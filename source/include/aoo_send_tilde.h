#pragma once

#include "ext.h"			// standard Max include
#include "ext_obex.h"		// required for new style Max object
#include "z_dsp.h"

#include "aoo.h"
#include "aoo_source.hpp"

#include "aoo_max_common.hpp"

#define DEFBUFSIZE 256



struct t_sink
{
    aoo::ip_address s_address;
    AooId s_id;
    t_symbol *s_group;
    t_symbol *s_user;
};

// struct to represent the object's state
// not a typedef struct anymore
struct t_aoo_send {
	t_aoo_send(int argc, t_atom *argv);
    ~t_aoo_send();

	t_pxobject		obj;			// the object itself (t_pxobject in MSP instead of t_object)

    int32_t			x_nchannels = 0;
    void 			*x_msgout = nullptr;
    std::unique_ptr<t_double *[]> x_vec;


    AooSource::Ptr 	x_source;
    AooId 			x_id = 0;
    int32_t         x_port = 0;
    t_node          *x_node = nullptr;
    AooId x_invite_token = kAooIdInvalid;
    bool x_auto_invite = true; // on by default
    bool x_running = false;
    double x_logicaltime = 0;
    double x_samplerate = 0;
    long x_blocksize = 0;

    bool x_valid = true; // if the object is not properly constructed 


    std::vector<t_sink> x_sinks;

    t_symbol 		*x_codec = nullptr;

    t_clock 		*x_clock = nullptr;
    bool			x_multi = false;

    void add_sink(const aoo::ip_address& addr, AooId id);
    const t_sink *find_sink(const aoo::ip_address& addr, AooId id) const;
    void remove_sink(const aoo::ip_address& addr, AooId id);
    bool check(const char *name) const;
    bool check(int argc, t_atom *argv, int minargs, const char *name) const;
    bool get_sink_arg(int argc, t_atom *argv, aoo::ip_address& addr, AooId& id, bool check) const;
    void remove_all();
};

static void aoo_send_handle_event(t_aoo_send *x, const AooEvent *event, int32_t);

// MAX MSP method prototypes
void *aoo_send_new(t_symbol *s, long argc, t_atom *argv);
void aoo_send_free(t_aoo_send *x);
void aoo_send_assist(t_aoo_send *x, void *b, long m, long a, char *s);
void aoo_send_dsp64(t_aoo_send *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void aoo_send_perform64(t_aoo_send *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
