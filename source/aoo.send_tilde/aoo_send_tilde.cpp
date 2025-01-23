/**
	@file
	aoo.send~: a simple audio object for Max
	original by: Christof Ressi, ported to Max bz Davide Gagliardi
	@ingroup network
*/
#include "aoo_send_tilde.h"

static void aoo_send_tick(t_aoo_send *x)
{
    x->x_source->pollEvents();
}
/**
 * @brief // how to handle the incoming events
 * 
 * @param x the object
 * @param event the type of the event
 */
static void aoo_send_handle_event(t_aoo_send *x, const AooEvent *event, int32_t)
{
    switch (event->type){
    case kAooEventSinkPing: // all these cases react to the code defined for kAooEventFrameResend
    case kAooEventInvite:
    case kAooEventUninvite:
    case kAooEventSinkAdd:
    case kAooEventSinkRemove:
    case kAooEventFrameResend:
    {
        // common endpoint header
        auto& ep = event->endpoint.endpoint;
        aoo::ip_address addr((const sockaddr *)ep.address, ep.addrlen);
        t_atom msg[12];
        // once it has the id and the ip address of the endpoint, it serializes the endpoint
        if (!x->x_node->serialize_endpoint(addr, ep.id, 3, msg)) {
            error("BUG: aoo_send_handle_event: serialize_endpoint");
            return;
        }
        // event data
        switch (event->type){
        case kAooEventInvite:
        {
            auto& e = event->invite;

            x->x_invite_token = e.token;
            if (x->x_auto_invite) {
                // accept by default if streaming; decline otherwise
                x->x_source->handleInvite(ep, e.token, x->x_running);
                if (!x->x_running) {
                    return; // don't send "invite" event!
                }
            }
            // Send "invite" event.
            // In manual mode, the user is supposed to handle the invitation;
            // in auto mode, we want to tell the user that the sink has been
            // activated (together with the metadata).
            if (e.metadata) {
                // <ip> <port> <id> <type> <data...>
                auto count = 4 + (e.metadata->size / datatype_element_size(e.metadata->type));
                t_atom *vec = (t_atom *)alloca(count * sizeof(t_atom));
                // copy endpoint
                memcpy(vec, msg, 3 * sizeof(t_atom));
                // copy data
                data_to_atoms(*e.metadata, count - 3, vec + 3);

                outlet_anything(x->x_msgout, gensym("invite"), count, vec);
            } else {
                outlet_anything(x->x_msgout, gensym("invite"), 3, msg);
            }
            break;
        }
        case kAooEventUninvite:
        {
            x->x_invite_token = event->uninvite.token;
            if (x->x_auto_invite) {
                // accept by default
                x->x_source->handleUninvite(ep, event->uninvite.token, true);
            }
            // Send "uninvite" event.
            // In manual mode, the user is supposed to handle the uninvitation;
            // in auto mode, we want to tell the user that the sink has been
            // deactivated.
            outlet_anything(x->x_msgout, gensym("uninvite"), 3, msg);

            break;
        }
        case kAooEventSinkAdd:
        {
            if (!x->find_sink(addr, ep.id)){
                x->add_sink(addr, ep.id);
            } else {
                // the sink might have been added concurrently by the user (very unlikely)
                object_post((t_object*)x, "sink %s %d %d already added", addr.name(), addr.port(), ep.id);
            }
            break;
        }
        case kAooEventSinkRemove:
        {
            if (x->find_sink(addr, ep.id)){
                x->remove_sink(addr, ep.id);
            } else {
                // the sink might have been removed concurrently by the user (very unlikely)
                object_post((t_object *)x, "sink %s %d %d already removed", addr.name(), addr.port(), ep.id);
            }
            break;
        }
        //--------------------- sink events -----------------------//
        case kAooEventSinkPing:
        {
            auto& e = event->sinkPing;

            double delta1 = aoo_ntpTimeDuration(e.t1, e.t2) * 1000.0;
            double delta2 = aoo_ntpTimeDuration(e.t3, e.t4) * 1000.0;
            double total_rtt = aoo_ntpTimeDuration(e.t1, e.t4) * 1000.0;
            double network_rtt = total_rtt - aoo_ntpTimeDuration(e.t2, e.t3) * 1000;

            atom_setfloat(msg + 3, delta1);
            atom_setfloat(msg + 4, delta2);
            atom_setfloat(msg + 5, network_rtt);
            atom_setfloat(msg + 6, total_rtt);
            atom_setfloat(msg + 7, e.packetLoss);

            outlet_anything(x->x_msgout, gensym("ping"), 8, msg);

            break;
        }
        case kAooEventFrameResend:
        {
            atom_setfloat(msg + 3, event->frameResend.count);
            outlet_anything(x->x_msgout, gensym("frame_resent"), 4, msg);
            break;
        }
        default:
            error("BUG: aoo_send_handle_event: bad case label!");
            break;
        }

        break; // !
    }
    default:
        object_post((t_object*)x, "unknown event type (%d)", event->type);
        break;
    }
}
/**
 * @brief initialize x_node with the given port and id
 * 
 * @param x l'oggetto
 * @param f1 la porta
 * @param f2 l'id
 */
static void aoo_send_set(t_aoo_send *x, int f1, int f2)
{
    int port = f1;
    AooId id = f2;

    if (id == x->x_id && port == x->x_port) {
        return;
    }

    if (id < 0) {
        object_error((t_object*)x, "bad id %d", id);
        return;
    }

    if (port < 0) {
        // NB: 0 is allowed (= don't listen)!
        object_error((t_object*)x, "bad port %d", id);
        return;
    }

    // always release node!
    if (x->x_node) {
        x->x_node->release((t_object*)x, x->x_source.get());
    }

    if (id != x->x_id) {
        x->x_source->setId(id);
        x->x_id = id;
    }
    // given the port inizialize a network node
    if (port) {
        x->x_node = t_node::get((t_object* )x, port, x->x_source.get(), id);
    } else {
        x->x_node = nullptr;
    }
    x->x_port = port;
    // object_post((t_object*)x, "port %d id %d", x->x_port, x->x_id);
}
/**
 * @brief alias for aoo_send_set
 * 
 * @param x l'oggetto t_aoo_send
 * @param f la porta
 */
static void aoo_send_port(t_aoo_send *x, int f)
{
    aoo_send_set(x, f, x->x_id);
}
/**
 * @brief Construct a new t aoo send::t aoo send object
 * 
 * @param argc 
 * @param argv num channels (inlets) - port - id
 */
t_aoo_send::t_aoo_send(int argc, t_atom *argv)
{
    x_clock = clock_new(this, (method)aoo_send_tick);

    // flags multichannel support
    // while (argc && argv->a_type == A_SYM) {
    //     auto flag = argv->a_w.w_sym->s_name;
    //     if (*flag == '-') {
    //         if (!strcmp(flag, "-m")) {
    //             if (g_signal_setmultiout) {
    //                 x_multi = true;
    //             } else {
    //                 pd_error(this, "%s: no multi-channel support, ignoring '-m' flag", classname(this));
    //             }
    //         } else {
    //             pd_error(this, "%s: ignore unknown flag '%s",
    //                      classname(this), flag);
    //         }
    //         argc--; argv++;
    //     } else {
    //         break;
    //     }
    // }

    // arg #1: channels
    int ninlets;
    if (x_multi) {
        // one multi-channel inlet
        ninlets = 1;
        // x_nchannels is used to keep track of the channel count, see "dsp" method.
        // The creation argument sets the initial number of channels, so we can set
        // the default format. NB: the channel count cannot be zero!
        x_nchannels = std::max<int>(atom_getfloatarg(0, argc, argv), 1);
    } else {
        // NB: users may explicitly specify 0 channels for pure message streams!
        // (In this case, the user must provide the number of "message channels"
        // - if needed - with the "format" message.)
        ninlets = argc > 0 ? atom_getfloat(argv) : 1;
        if (ninlets < 0){
            ninlets = 0;
        } else if (ninlets > AOO_MAX_NUM_CHANNELS) {
            // NB: in theory we can support any number of channels;
            // this rather meant to handle patches that accidentally
            // use the old argument order where the port would come first!
            object_error((t_object*)this, "channel count (%d) out of range", ninlets);
            ninlets = 0;
        }
        x_nchannels = ninlets;
    }

    // arg #2 (optional): port number
    // NB: 0 means "don't listen"
    int port = atom_getintarg(1, argc, argv);

    // arg #3 (optional): ID
    AooId id = atom_getintarg(2, argc, argv);
    if (id < 0) {
        object_error((t_object*)this, "bad id % d, setting to 0", id);
        id = 0;
    }
    x_id = id;

    // make additional inlets
    dsp_setup((t_pxobject *)this, ninlets);	// MSP inlets: arg is # of inlets and is REQUIRED!

    // channel vector
    if (x_nchannels > 0) {
        x_vec = std::make_unique<t_sample *[]>(x_nchannels);
    }
    // make event outlet
    x_msgout = outlet_new(this, 0);

    // create and initialize AooSource object
    x_source = AooSource::create(x_id);

    // set event handler
    x_source->setEventHandler((AooEventHandler)aoo_send_handle_event, this, kAooEventModePoll);

    // set default format
    AooFormatStorage fmt;
    format_makedefault(fmt, x_nchannels);
    x_source->setFormat(fmt.header);
    x_codec = gensym(fmt.header.codecName);

    x_source->setBufferSize(DEFBUFSIZE * 0.001);
    // finally we're ready to receive messages
    aoo_send_port(this, port);
}
/**
 * @brief Destroy the t aoo send::t aoo send object and release the network node
 * 
 */
t_aoo_send::~t_aoo_send()
{
     // first stop receiving messages
    if (x_node){
        x_node->release((t_object *)this, x_source.get());
    }

    clock_free(x_clock);
}
/**
 * @brief add a sink in the list of sinks available
 * 
 * The function is called from aoo_send_add into which
 * an endpoint is created in the network. At this point 
 * this function updates the x_sink register of the aoo.send~
 * instance and informs the Max user in case of success
 * 
 * @param addr 
 * @param id 
 */
void t_aoo_send::add_sink(const aoo::ip_address& addr, AooId id)
{
    // add sink to list; try to find peer name!
    t_symbol *group = nullptr;
    t_symbol *user = nullptr;
    x_node->find_peer(addr, group, user);
    x_sinks.push_back({addr, id, group, user});

    // output message
    t_atom msg[3];
    if (x_node->serialize_endpoint(addr, id, 3, msg)){
        outlet_anything(x_msgout, gensym("add"), 3, msg);
    } else {
        error("BUG: t_aoo_send::add_sink: serialize_endpoint");
    }
}
/**
 * @brief check if the sink is registered in the x_sinks vector
 * 
 * @param addr 
 * @param id 
 * @return const t_sink* 
 */
const t_sink *t_aoo_send::find_sink(const aoo::ip_address& addr, AooId id) const
{
    for (auto& sink : x_sinks){
        if (sink.s_address == addr && sink.s_id == id){
            return &sink;
        }
    }
    return nullptr;
}
/**
 * @brief remove a sink from the x_sink vector
 * 
 * @param addr 
 * @param id 
 */
void t_aoo_send::remove_sink(const aoo::ip_address& addr, AooId id)
{
    // remove the sink matching endpoint and id
    for (auto it = x_sinks.begin(); it != x_sinks.end(); ++it){
        if (it->s_address == addr && it->s_id == id){
            x_sinks.erase(it);

            // output message
            t_atom msg[3];
            if (x_node->serialize_endpoint(addr, id, 3, msg)){
                outlet_anything(x_msgout, gensym("remove"), 3, msg);
            } else {
                error("BUG: aoo_send_doremovesink: serialize_endpoint");
            }
            return;
        }
    }
    error("BUG: t_aoo_send::remove_sink");
}
/**
 * @brief check if the object has a node registered
 * 
 * @param name 
 * @return true 
 * @return false 
 */
bool t_aoo_send::check(const char *name) const
{
    if (x_node){
        return true;
    } else {
        object_error((t_object*)this, "'%s' failed: no socket!", name);
        return false;
    }
}
/**
 * @brief check if the "add" message is properly formatted
 * 
 * @param argc 
 * @param argv 
 * @param minargs 
 * @param name 
 * @return true 
 * @return false 
 */
bool t_aoo_send::check(int argc, t_atom *argv, int minargs, const char *name) const
{
    if (!check(name)) return false;

    if (argc < minargs){
        object_error((t_object*)this, "too few arguments for '%s' message", name);
        return false;
    }

    return true;
}
/**
 * @brief Find a peer in x_sinks by group and user or by ip and port
 * 
 * @param argc 
 * @param argv the message "add" from the user
 * @param addr an ip address
 * @param id an id
 * @param check 
 * @return true 
 * @return false 
 */
bool t_aoo_send::get_sink_arg(int argc, t_atom *argv,
                              aoo::ip_address& addr, AooId& id, bool check) const
{
    // if there is no registered node - STOP
    if (!x_node) {
        object_error((t_object*)this, "no socket!");
        return false;
    }
    // if the message in not properly formatted - STOP
    if (argc < 3){
        object_error((t_object*)this, "too few arguments for sink");
        return false;
    }
    // first try peer (group|user)
    if (argv[1].a_type == A_SYM) {
        t_symbol *group = atom_getsym(argv);
        t_symbol *user = atom_getsym(argv + 1);
        id = atom_getfloat(argv + 2);
        // first search sink list, in case the client has been disconnected
        for (auto& s : x_sinks) {
            if (s.s_group == group && s.s_user == user && s.s_id == id) {
                addr = s.s_address;
                return true;
            }
        }
        if (!check) {
            // not yet in list -> try to get from client
            if (x_node->find_peer(group, user, addr)) {
                return true;
            }
        }
        object_error((t_object*)this, "couldn't find sink %s|%s %d", group->s_name, user->s_name, id);
        return false;
    } else {
        // otherwise try host|port
        t_symbol *host = atom_getsym(argv);
        int port = atom_getfloat(argv + 1);
        id = atom_getfloat(argv + 2);
        if (x_node->resolve(host, port, addr)) {
            if (check) {
                // try to find in list
                for (auto& s : x_sinks) {
                    if (s.s_address == addr && s.s_id == id) {
                        return true;
                    }
                }
                object_error((t_object*)this, "couldn't find sink %s %d %d", host->s_name, port, id);
                return false;
            } else {
                return true;
            }
        } else {
            object_error((t_object*)this, "couldn't resolve sink hostname '%s'", host->s_name);
            return false;
        }
    }
}
/**
 * @brief remove all sinks in the network
 * 
 */
void t_aoo_send::remove_all()
{
    x_source->removeAll();

    int numsinks = x_sinks.size();
    if (!numsinks){
        return;
    }

    // temporary copies (for reentrancy)
    t_sink *sinks = (t_sink *)alloca(sizeof(t_sink) * numsinks);
    std::copy(x_sinks.begin(), x_sinks.end(), sinks);

    x_sinks.clear();

    // output messages
    for (int i = 0; i < numsinks; ++i){
        t_atom msg[3];
        if (x_node->serialize_endpoint(sinks[i].s_address, sinks[i].s_id, 3, msg)){
            outlet_anything(x_msgout, gensym("remove"), 3, msg);
        } else {
            error("BUG: aoo_send_doremoveall: serialize_endpoint");
        }
    }
}
//********************** object methods **********************
/**
 * @brief Connect to a node
 * 
 * @param x the object
 * @param s 
 * @param argc 
 * @param argv 
 */
static void aoo_send_add(t_aoo_send *x, t_symbol *s, int argc, t_atom *argv)
{
    // check that the add message is properly formatted
    if (!x->check(argc, argv, 3, "add")) return;

    aoo::ip_address addr;
    AooId id;
    if (x->get_sink_arg(argc, argv, addr, id, false)){
        // check if sink exists
        if (x->find_sink(addr, id)) {
            if (argv[1].a_type == A_SYM){
                // group + user
                auto group = atom_getsym(argv)->s_name;
                auto user = atom_getsym(argv + 1)->s_name;
                object_error((t_object*)x, "sink %s|%s %d already added!", group, user, id);
            } else {
                // host + port
                auto host = atom_getsym(argv)->s_name;
                object_error((t_object*)x, "sink %s %d %d already added!", host, addr.port(), id);
            }
            return;
        }
        // if not in x_sinks create one (an endpoint)
        AooEndpoint ep { addr.address(), (AooAddrSize)addr.length(), id };
        // it should be possible to add another bool (0/1) argument to the add message 
        bool active = argc > 3 ? atom_getfloat(argv + 3) : true;
        // add a sink to the network
        x->x_source->addSink(ep, active);

#if 0
        // not yet implemented
        if (argc > 4) {
            int channel = atom_getfloat(argv + 4);
            x->x_source->setSinkChannelOffset(ep, channel);
        }
        if (argc > 5) {
            int channel = atom_getfloat(argv + 5);
            x->x_source->setSinkChannelOffset(ep, channel);
        }
#endif

        x->add_sink(addr, id);

        // print message (use actual IP address)
        object_post((t_object*)x, "added sink %d %d", addr.name(), addr.port(), id);
    }
}
/**
 * @brief disconnect from a node
 * 
 * @param x the object
 * @param s 
 * @param argc arg count
 * @param argv the message containing the info of the node to remove
 */
static void aoo_send_remove(t_aoo_send *x, t_symbol *s, int argc, t_atom *argv)
{
    // check if the object has a node
    if (!x->check("remove")) return;
    // if the message is just "remove" remove
    if (!argc){
        x->remove_all();
        return;
    }

    if (argc < 3){
        object_error((t_object*)x, "too few arguments for 'remove' message");
        return;
    }

    aoo::ip_address addr;
    AooId id;
    if (x->get_sink_arg(argc, argv, addr, id, true)) {
        AooEndpoint ep { addr.address(), (AooAddrSize)addr.length(), id };
        x->x_source->removeSink(ep);

        x->remove_sink(addr, id);

        object_post((t_object*)x, "removed sink %d %d %d", addr.name(), addr.port(), id);
    }
}

static void aoo_send_start(t_aoo_send *x, t_symbol *s, int argc, t_atom *argv)
{
    int32_t offset = (gettime()- x->x_logicaltime) * 0.001 * (int32_t)x->x_samplerate;
    // in case "start" is sent while DSP is off...
    if (offset >= (int32_t)x->x_blocksize) {
        offset = 0;
    }
    if (argc > 0) {
        // with metadata
        AooData metadata;
        if (!atom_to_datatype(*argv, metadata.type, x)) {
            return;
        }
        argc--; argv++;
        if (!argc) {
            object_error((t_object*)x, "metadata must not be empty");
            return;
        }
        auto size = argc * datatype_element_size(metadata.type);
        auto data = (AooByte *)alloca(size);
        atoms_to_data(metadata.type, argc, argv, data, size);
        metadata.size = size;
        metadata.data = data;
        // start the stream
        x->x_source->startStream(offset, &metadata);
    } else {
        // start the stream
        x->x_source->startStream(offset, nullptr);
        object_post((t_object*)x, "Stream Started");
    }
    x->x_running = true;
}
static void aoo_send_stop(t_aoo_send *x)
{
    int32_t offset = (gettime()-x->x_logicaltime) * 0.001 * (int32_t)x->x_samplerate;
    // in case "stop" is sent while DSP is off...
    if (offset >= (int32_t)x->x_blocksize) {
        offset = 0;
    }
    x->x_source->stopStream(offset);
    object_post((t_object*)x, "Stream Stopped");
    x->x_running = false;
}
static void aoo_send_ping(t_aoo_send *x, double f)
{
    x->x_source->setPingInterval(f * 0.001);
}
static void aoo_send_sink_list(t_aoo_send *x)
{
    if (!x->check("sink_list")) return;

    for (auto& sink : x->x_sinks){
        t_atom msg[3];
        if (x->x_node->serialize_endpoint(sink.s_address, sink.s_id, 3, msg)){
            outlet_anything(x->x_msgout, gensym("sink"), 3, msg);
        } else {
            error("BUG: t_node::serialize_endpoint");
        }
    }
}
//***********************************************************************************************

void ext_main(void *r)
{
	// object initialization, note the use of dsp_free for the freemethod, which is required
	// unless you need to free allocated memory, in which case you should call dsp_free from
	// your custom free function.

	t_class *c = class_new("aoo.send~", (method)aoo_send_new, (method)aoo_send_free, (long)sizeof(t_aoo_send), 0L, A_GIMME, 0);

	class_addmethod(c, (method)aoo_send_dsp64,		"dsp64",	A_CANT, 0);
	class_addmethod(c, (method)aoo_send_assist,	"assist",	A_CANT, 0);

    class_addmethod(c, (method)aoo_send_add, "add", A_GIMME, 0);
    class_addmethod(c, (method)aoo_send_remove,"remove", A_GIMME, 0);
    class_addmethod(c, (method)aoo_send_start, "start", A_GIMME, 0);
    class_addmethod(c, (method)aoo_send_stop, "stop", 0);

    class_addmethod(c, (method)aoo_send_ping,"ping", A_FLOAT, 0);

    class_addmethod(c, (method)aoo_send_sink_list, "sink_list", 0);
	// initializes class dsp methods for audio processing
	class_dspinit(c);
	// registers the class as an object which can be instantiated in a Max patch
	class_register(CLASS_BOX, c);
	aoo_send_class = c;

    aoo_node_setup();
}

void *aoo_send_new(t_symbol *s, long argc, t_atom *argv)
{
	t_aoo_send *x = (t_aoo_send *)object_alloc(aoo_send_class);

	// if (x) {
		new (x) t_aoo_send(argc, argv);
		// use 0 if you don't need inlets
		// aoo.send~ has no signal outlet
		// outlet_new(x, "signal"); 		// signal outlet (note "signal" rather than NULL)
		// x->offset = 0.0;
	// }
	return (x);
}

void aoo_send_free(t_aoo_send *x)
{
	x->~t_aoo_send();
}

void aoo_send_assist(t_aoo_send *x, void *b, long m, long a, char *s)
{
	// if (m == ASSIST_INLET) { //inlet
	// 	sprintf(s, "I am inlet %ld", a);
	// }
	// else {	// outlet
	// 	sprintf(s, "I am outlet %ld", a);
	// }
    ;
}

// registers a function for the signal chain in Max
void aoo_send_dsp64(t_aoo_send *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
    int32_t nchannels = x->x_nchannels;    
	
    if(maxvectorsize != x->x_blocksize || samplerate != x->x_samplerate){
        x->x_source->setup(nchannels, samplerate, (int32_t) maxvectorsize, kAooFixedBlockSize);
        x->x_blocksize = maxvectorsize;
        x->x_samplerate = samplerate;
    }

	// instead of calling dsp_add(), we send the "dsp_add64" message to the object representing the dsp chain
	// the arguments passed are:
	// 1: the dsp64 object passed-in by the calling function
	// 2: the symbol of the "dsp_add64" message we are sending
	// 3: a pointer to your object
	// 4: a pointer to your 64-bit perform method
	// 5: flags to alter how the signal chain handles your object -- just pass 0
	// 6: a generic pointer that you can use to pass any additional data to your perform method

	object_method(dsp64, gensym("dsp_add64"), x, aoo_send_perform64, 0, NULL);
}

// this is the 64-bit perform method audio vectors
void aoo_send_perform64(t_aoo_send *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    static_assert(sizeof(t_sample) == sizeof(AooSample), "AooSample size must match t_sample");

    if(x->x_node){
        auto err = x->x_source->process(ins, (int32_t)sampleframes, get_osctime());
        
        if(err == kAooErrorOverflow){
            object_error((t_object*)x, "send buffer overflow. Try to manually increase "
                        "the send buffer size with the 'buffersize' method.");
        }

        if(err != kAooErrorIdle){
            x->x_node->notify();
        }

        if(x->x_source->eventsAvailable()){
            post("AVAILABLEE");
            clock_delay(x->x_clock, 0);
        }

        x->x_logicaltime = gettime();

    }

	// t_double *inL = ins[0];		// we get audio for each inlet of the object from the **ins argument
	// t_double *outL = outs[0];	// we get audio for each outlet of the object from the **outs argument
	// int n = sampleframes;

	// // this perform method simply copies the input to the output, offsetting the value
	// while (n--)
	// 	// *outL++ = x->offset;
	// 	*outL++ = *inL++ + x->offset;
}

