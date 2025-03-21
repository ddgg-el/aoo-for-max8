#include "ext_critical.h"
#include "aoo_max_node.h"

#include "common/log.hpp"
#include "common/sync.hpp"

t_class *aoo_node_proxy_class = nullptr;

/**
 * @brief find or create a node that can be assigned to an object
 * 
 * @param obj the object to which it will be assigned
 * @param port the port
 * @param x an AooClient
 * @param id the id
 * @return t_node* 
 */
t_node * t_node::get(t_object *obj, int port, void *x, AooId id)
{
    t_node_imp *node = nullptr;
    // make bind symbol for port number
    char buf[64];
    snprintf(buf, sizeof(buf), "aoo_node %d", port);
    t_symbol *s = gensym(buf);
    // find or create node
    auto y = (t_node_proxy *)object_findregistered(aoo_node_proxy_class->c_sym, s);
    if (y) {
        node = y->x_node;
    } else {
        try {
            // finally create aoo node instance            
            object_new(CLASS_NOBOX, aoo_node_proxy_class->c_sym, s, port);
            auto proxy = (t_node_proxy *)object_findregistered(aoo_node_proxy_class->c_sym, s);
            node = proxy->x_node;
        } catch (const std::exception& e) {
            object_error((t_object*)obj, "%s", e.what());
            return nullptr;
        }
    }
    // connect the object to the node
    if (!node->add_object(obj, x, id)){
        // never fails for new t_node_imp!
        return nullptr;
    }

    return node;
}

/**
 * @brief Construct a new t_node_imp object
 * 
 * @param s a symbol "aoo_node <port>"
 * @param port 
 */
// instantiate a node | a node is a wrapper around an AooClient
t_node_imp::t_node_imp(t_symbol *s, int port, t_node_proxy* obj)
    : x_obj(obj), x_bindsym(s), x_port(port)
{
    auto client = AooClient::create();

    client->setEventHandler([](void *user, const AooEvent *event, AooThreadLevel level) {
        auto x = static_cast<t_node_imp *>(user);
        if (x->x_clientobj) {
            aoo_client_handle_event((t_aoo_client *)x->x_clientobj, event, level);
        }
    }, this, kAooEventModePoll);

    AooClientSettings settings;
    settings.portNumber = port;
    if (auto err = client->setup(settings); err != kAooOk) {
        std::string msg;
        if (err == kAooErrorSocket) {
            msg = aoo::socket::strerror(aoo::socket::get_last_error());
        } else {
            msg = aoo_strerror(err);
        }
        object_error((t_object*)this->x_clientobj, "Could not create node: %s", msg.c_str());
        // FIX
        throw std::runtime_error("could not create node: " + msg);
    }
    // get socket type
    if (settings.socketType & kAooSocketIPv6) {
        x_type = aoo::ip_address::IPv6;
    } else {
        x_type = aoo::ip_address::IPv4;
    }
    x_ipv4mapped = settings.socketType & kAooSocketIPv4Mapped;

    // success
    x_client = std::move(client);
        


#if NETWORK_THREAD_POLL
    // start network I/O thread
    LOG_DEBUG("start network thread");
    x_iothread = std::thread([this, pd=pd_this]() {
    #ifdef PDINSTANCE
        pd_setinstance(pd);
    #endif
        aoo::sync::lower_thread_priority();
        perform_io();
    });
#else
    t_max_err err;
    // start send thread
    err = systhread_create((method)send, this, 0, 0, 0, &x_sendthread);
    if(err != MAX_ERR_NONE){
        object_error((t_object*)this, "Could not create send thread");
    }
    // start receive thread
    err = systhread_create((method)receive, this, 0, 0, 0, &x_recvthread);
    if(err != MAX_ERR_NONE){
        object_error((t_object*)this, "Could not create receive thread");
    }
#endif

    object_register(aoo_node_proxy_class->c_sym, x_bindsym, obj);
    object_post(nullptr, "aoo: new node on port %d", port);
}

// TODO: documenta
t_node_imp::~t_node_imp()
{
    object_unregister(x_obj);
    // pd_unbind(&x_proxy.x_pd, x_bindsym);
    unsigned int c_ret;
    unsigned int s_ret;
    unsigned int r_ret;
    // stop the client and join threads
    x_client->stop();
    if (x_clientthread){
        systhread_join(x_clientthread, &c_ret);
    }
#if NETWORK_THREAD_POLL
    LOG_DEBUG("join network thread");
    // notify and join I/O thread
    x_quit.store(true);
    if (x_iothread.joinable()) {
        x_iothread.join();
    }
#else
    LOG_DEBUG("join network threads");
    // join both threads
    
    if (x_sendthread) {
        systhread_join(x_sendthread, &s_ret);
    }
    if (x_recvthread) {
        systhread_join(x_recvthread, &r_ret);
    }
#endif

    object_post(nullptr, "aoo: released node on port %d", x_port);
}
 
/**
 * @brief given an address, ask the client find the peer group and user
 * 
 * @param addr 
 * @param group 
 * @param user 
 * @return true 
 * @return false 
 */
bool t_node_imp::find_peer(const aoo::ip_address& addr,
                           t_symbol *& group, t_symbol *& user) const {
    if (x_clientobj &&
            aoo_client_find_peer(
               (t_aoo_client *)x_clientobj, addr, group, user)) {
        return true;
    } else {
        return false;
    }
}

/**
 * @brief ask the client to find a peer in the network given group and username
 * 
 * @param group 
 * @param user 
 * @param addr 
 * @return true 
 * @return false 
 */
bool t_node_imp::find_peer(t_symbol * group, t_symbol * user,
                           aoo::ip_address& addr) const {
    if (x_clientobj &&
            aoo_client_find_peer(       
                (t_aoo_client *)x_clientobj, group, user, addr)) {
        return true;
    } else {
        return false;
    }
}

/**
 * @brief prepare the message to inform the Max user
 * 
 * a new endpoint has been registered or removed. Format the infos 
 * about this endpoint to send a message via the info outlet
 * 
 * @param addr 
 * @param id 
 * @param argc 
 * @param argv 
 * @return int 
 */
int t_node_imp::serialize_endpoint(const aoo::ip_address &addr, AooId id,
                                   int argc, t_atom *argv) const {
    if (argc < 3 || !addr.valid()) {
        LOG_DEBUG("serialize_endpoint: invalid address");
        return 0;
    }
    t_symbol *group, *user;
    // try to find a peer with a matching address/group
    if (find_peer(addr, group, user)) {
        // write group name, user name, id in msg
        atom_setsym(argv, group);
        atom_setsym(argv + 1, user);
        atom_setfloat(argv + 2, id);
    } else {
        // wirte ip string, port number, id in msg
        atom_setsym(argv, gensym(addr.name()));
        atom_setfloat(argv + 1, addr.port());
        atom_setfloat(argv + 2, id);
    }
    return 3;
}
// TODO: documenta
void t_node_imp::run_client(t_node_imp *x) {
    aoo::sync::lower_thread_priority();
    auto err = x->x_client->run(kAooInfinite);
    if (err != kAooOk) {
        std::string msg;
        if (err == kAooErrorSocket) {
            msg = aoo::socket::strerror(aoo::socket::get_last_error());
        } else {
            msg = aoo_strerror(err);
        }
        critical_enter(0);
        object_error((t_object*)x->x_clientobj, "TCP error: %s", msg.c_str());
        // TODO: handle error
        critical_exit(0);
    }
}

/**
 * @brief 
 * 
 * @param obj either an aoo_client, aoo_send or aoo_receive
 * @param x an AooClient
 * @param id an id
 * @return true 
 * @return false 
 */
bool t_node_imp::add_object(t_object *obj, void *x, AooId id)
{
    t_class * obj_class = object_class(obj);

    if (obj_class->c_sym == gensym("aoo.client")){
        // aoo_client
        if (!x_clientobj){
            x_clientobj = (t_object*)obj;
            // start thread lazily
            t_max_err err = systhread_create((method)run_client, this, 0, 0, 0, &x_clientthread);
            if(err != MAX_ERR_NONE){
                object_error((t_object*)obj, "Could not create client thread");
            }
        } else {
            object_error((t_object*)obj, "on port %d already exists!", port());
            return false;
        }
    }

    else  if (obj_class->c_sym == gensym("aoo.send~")){
        if (x_client->addSource((AooSource *)x) != kAooOk){
            object_error((t_object*)obj, "with ID %d on port %d already exists!", id, port());
        }
    }
    else if (obj_class->c_sym == gensym("aoo.receive~")){
        if (x_client->addSink((AooSink *)x) != kAooOk){
            object_error((t_object*)obj, "with ID %d on port %d already exists!", id, port());
        }
    }
    else {
        error("BUG: t_node_imp: bad client");
        return false;
    }
    x_refcount++;
    return true;
}

/**
 * @brief release the network node
 * 
 * @param obj the deleted external
 * @param x an AooSource
 */
void t_node_imp::release(t_object *obj, void *x)
{
    t_class * obj_class = object_class(obj);
    if (obj_class->c_sym == gensym("aoo.client")){
        // client
        x_clientobj = nullptr;
    }
    else if (obj_class->c_sym == gensym("aoo.send~")){
        x_client->removeSource((AooSource *)x);
    } 
    else if (obj_class->c_sym == gensym("aoo.receive~")){
        x_client->removeSink((AooSink *)x);
    }
    else {
        error("BUG: t_node_imp::release");
        return;
    }

    if (--x_refcount == 0){
        // last instance
        object_free(this->x_obj);

    } else if (x_refcount < 0){
        error("BUG: t_node_imp::release: negative refcount!");
    }
}

/**
 * @brief find a peer using aoo::ip_address::resolve
 * 
 * @param host 
 * @param port 
 * @param addr 
 * @return true 
 * @return false 
 */
bool t_node_imp::resolve(t_symbol *host, int port, aoo::ip_address& addr) const
{
    try {
        auto result = aoo::ip_address::resolve(host->s_name, port, x_type, x_ipv4mapped);
        assert(!result.empty());
        addr = result.front();
        return true;
    } catch (const aoo::resolve_error& e) {
        error("%s", e.what());
        return false;
    }
}

#if NETWORK_THREAD_POLL
void t_node_imp::perform_io() {
    auto check_error = [this](auto err) {
        if (err != kAooOk && err != kAooErrorWouldBlock) {
            std::string msg;
            if (err == kAooErrorSocket) {
                msg = aoo::socket_strerror(aoo::socket_errno());
            } else {
                msg = aoo_strerror(err);
            }
            sys_lock();
            pd_error(x_clientobj, "%s: UDP error: %s",
                     (x_clientobj ? "aoo_client" : "aoo"), msg.c_str());
            // TODO: handle error
            sys_unlock();
            return false;
        } else {
            return true;
        }
    };

    while (!x_quit.load(std::memory_order_relaxed)) {
        auto t1 = aoo::time_tag::now();

        auto err1 = x_client->receive(0);
        if (!check_error(err1)) {
            break;
        }

        auto err2 = x_client->send(0);
        if (!check_error(err2)) {
            break;
        }

        if (err1 == kAooErrorWouldBlock && err2 == kAooErrorWouldBlock) {
            // sleep
            auto t2 = aoo::time_tag::now();
            auto remaining = aoo::time_tag::duration(t1, t2) - POLL_INTERVAL;
            auto dur = std::chrono::duration<double>(remaining);
            std::this_thread::sleep_for(dur);
        }
    }
}
#else
void t_node_imp::send(t_node_imp *x) {
    aoo::sync::lower_thread_priority();
    auto err = x->x_client->send(kAooInfinite);
    if (err != kAooOk) {
        std::string msg;
        if (err == kAooErrorSocket) {
            msg = aoo::socket::strerror(aoo::socket::get_last_error());
        } else {
            msg = aoo_strerror(err);
        }
        critical_enter(0);
        object_error(x->x_clientobj, "UDP send error: %s", msg.c_str());
        // TODO: handle error
        critical_exit(0);
    }
}

void t_node_imp::receive(t_node_imp *x) {
    aoo::sync::lower_thread_priority();
    auto err = x->x_client->receive(kAooInfinite);
    if (err != kAooOk) {
        std::string msg;
        if (err == kAooErrorSocket) {
            msg = aoo::socket::strerror(aoo::socket::get_last_error());
        } else {
            msg = aoo_strerror(err);
        }
        critical_enter(0);
        object_error(x->x_clientobj, "UDP receive error: %s", msg.c_str());
        // TODO: handle error
        critical_exit(0);
    }
}
#endif

/**
 * @brief Creating the aoo_node class
 * 
 */
void aoo_node_setup(void)
{
    t_class *c;
    c = class_new("aoo_node", (method)aoo_node_new, (method)aoo_node_free, sizeof(t_node_proxy), 0L, A_NOTHING, 0);
    class_register(CLASS_NOBOX, c);
    aoo_node_proxy_class = c;

}

void *aoo_node_new(t_symbol *s, int port)
{
    t_node_imp* node;
    t_node_proxy *x = (t_node_proxy*)object_alloc(aoo_node_proxy_class);
    node = new t_node_imp(s, port, x);
    x->x_node = node;
    // object_register(aoo_node_proxy_class->c_sym, s, x);

    return (x);
}

void aoo_node_free(t_node_proxy *x)
{
    x->x_node->~t_node_imp();
}

/*////////////////////// aoo node //////////////////*/

