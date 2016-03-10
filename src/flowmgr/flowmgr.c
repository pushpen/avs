/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#define _POSIX_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <re/re.h>

#include "avs.h"

#if USE_MEDIAENGINE
#include "avs_voe.h"
#if HAVE_VIDEO
#include "avs_vie.h"
#endif
#endif

#include "flowmgr.h"


static const char *software = AVS_PROJECT " " AVS_VERSION " (" ARCH "/" OS ")";


/* Call signalling URL bases */

#define VOLUME_TIMEOUT 100 /* ms between volume updates */


static const char *events[FLOWMGR_EVENT_MAX] = {
	"call.flow-add",
	"call.flow-delete",
	"call.flow-active",
	"call.remote-candidates-add",
	"call.remote-candidates-update",
	"call.remote-sdp"
};


static struct {
	bool inited;
	bool started;
	struct list flows;
	struct tls *dtls;
	struct mqueue *mq;
	struct tmr vol_tmr;
	char *name;
	bool using_voe;
	char *log_url;
	bool dualstack;
	char ifname[256];

	struct list flowmgrl;

	const char *cert;
	size_t cert_len;

} msys = {
	.inited = false,
	.started = false,
	.flows = LIST_INIT,
	.dtls = NULL,
	.mq = NULL,
};


static struct list aucodecl; // todo: temp, move to application ???
static struct list vidcodecl; // todo: temp, move to application ???


static int process_event(bool *hp, struct flowmgr *fm,
			 const char *ctype, const char *content, size_t clen,
			 bool replayed);


static int dns_init(struct dnsc **dnscp)
{
        struct sa nsv[16];
        uint32_t nsn;
        int err;

        nsn = ARRAY_SIZE(nsv);

        err = dns_srv_get(NULL, 0, nsv, &nsn);
        if (err) {
                error("dns srv get: %m\n", err);
                goto out;
        }

        err = dnsc_alloc(dnscp, NULL, nsv, nsn);
        if (err) {
                error("dnsc alloc: %m\n", err);
                goto out;
        }

 out:
        return err;
}


const char *flowmgr_mediacat_name(enum flowmgr_mcat mcat)
{
	switch (mcat) {

	case FLOWMGR_MCAT_NORMAL: return "Normal";
	case FLOWMGR_MCAT_HOLD:   return "Hold";
	case FLOWMGR_MCAT_ACTIVE: return "Active";
	case FLOWMGR_MCAT_CALL:   return "Call";
	default: return "???";
	}
}

int flowmgr_append_logmb(struct flowmgr *fm, struct call *call,
			 struct mbuf *mb)
{
	struct log_elem *logel;
	int err;

	if (!mb)
		return EINVAL;

	if (fm->log.appendh) {
		char *msg;

		err = mbuf_strdup(mb, &msg, mb->end);
		if (err)
			return err;

		fm->log.appendh(msg, fm->log.arg);
		mem_deref(msg);

		return 0;
	}

	err = log_elem_alloc(&logel);
	if (err)
		return err;

	logel->ts = tmr_jiffies();
	logel->mb = mem_ref(mb);

	if (call)
		list_append(call_logl(call), &logel->le, logel);
	else
		list_append(&fm->logl, &logel->le, logel);

	return 0;
}


int flowmgr_append_log(struct flowmgr *fm, struct call *call,
		       const char *fmt, ...)
{
	va_list ap;
	struct mbuf *mb;

	if (!fm->use_logging) 
		return 0;

	if (fm->log.appendh) {
		char *msg;

		va_start(ap, fmt);
		re_vsdprintf(&msg, fmt, ap);
		va_end(ap);

		fm->log.appendh(msg, fm->log.arg);
		mem_deref(msg);

		return 0;
	}

	mb = mbuf_alloc(1024);
	if (!mb)
		return ENOMEM;
	
	va_start(ap, fmt);

	mbuf_printf(mb, "%v", fmt, &ap);

	va_end(ap);

	flowmgr_append_logmb(fm, call, mb);
	mem_deref(mb);

	return 0;
}


int flowmgr_append_convlog(struct flowmgr *fm, const char *convid,
			   const char *msg)
{
	struct call *call;

	call = dict_lookup(fm->calls, convid);

	info("flowmgr(%p): append_convlog: convid=%s(%p) msg=%s\n",
	     fm, convid, call, msg);

	return flowmgr_append_log(fm, call, "%s", msg);
}


static const char *time_str(char *str, size_t slen, uint64_t ts)
{
	struct timeval tv;
	struct tm tm;
	char tstr[64];
	time_t t;

	tv.tv_sec = ts / 1000;
	tv.tv_usec = (suseconds_t)(ts - 
				   ((uint64_t)tv.tv_sec * (uint64_t)1000));

	t = (time_t)tv.tv_sec;
	gmtime_r(&t, &tm);

	strftime(tstr, sizeof(tstr), "%Y/%m/%d %H:%M:%S", &tm);

	re_snprintf(str, slen, "%s.%03d", tstr, tv.tv_usec);

	return str;
}


static void format_logl(struct list *logl, struct mbuf *mb)
{
	struct le *le = logl->head;
	char tstr[128];

	if (!mb)
		return;

	while(le) {
		struct log_elem *logel = le->data;

		le = le->next;

		mbuf_printf(mb, "%s %b\n",
			    time_str(tstr, sizeof(tstr), logel->ts),
			    logel->mb->buf, logel->mb->end);
	}
}


struct log_req {
	struct le le;
	struct http_req *http;
};


static void log_req_destructor(void *arg)
{
	struct log_req *req = arg;

	list_unlink(&req->le);
	mem_deref(req->http);
}


static void http_resp_handler(int err, const struct http_msg *msg, void *arg)
{
	struct log_req *req = arg;

	if (err) {
		warning("flowmgr: http_resp_handler: err=%d(%m) status=%d\n",
			err, err, msg ? msg->scode : 999);
	}
	else {
		info("flowmgr: http_resp_handler: log sent successfully\n");
	}

	mem_deref(req);
}


static void send_log(struct flowmgr *fm, struct call *call)
{
	struct mbuf *mb;
	char url[256];
	char dt[16];
	char mt[16];
	struct tm tm;
	struct log_req *req;
	time_t t;
	int err;

	if (!fm)
		return;

	if (fm->log.uploadh) {
		fm->log.uploadh(fm->log.arg);
		return;
	}
	
	if (!fm->httpc)
		return;

	if (!msys.log_url) {
		warning("send_log: no log_url set -- cannot log.\n");
		return;
	}

	mb = mbuf_alloc(1024);
	if (!mb)
		return;

	if (call) {
		struct list *logl = call_logl(call);

		format_logl(logl, mb);
		list_flush(logl);
	}

	if (fm) {
		format_logl(&fm->logl, mb);
		list_flush(&fm->logl);
	}

	time(&t);
	gmtime_r(&t, &tm);

	strftime(dt, sizeof(dt), "%d%m%Y", &tm);
	strftime(mt, sizeof(mt), "%H%M%S", &tm);
	re_snprintf(url, sizeof(url), "%s/%s/%s-%s.log",
		    msys.log_url, dt, flowmgr_call_sessid(call), mt);

	debug("flowmgr(%p): sending log to: %s len=%d\n",
	      fm, url, (int)mb->end);

	req = mem_zalloc(sizeof(*req), log_req_destructor);
	if (!req)
		goto out;

	list_append(&fm->logreql, &req->le, req);

	err = http_request(&req->http, fm->httpc, HTTP_PUT, url,
			   http_resp_handler, NULL, req,
			   "x-amz-acl: bucket-owner-full-control\r\n"
			   "Content-Type: application/x-octet-stream\r\n"
			   "Content-Length: %zu\r\n"
			   "\r\n"
			   "%b",
			   mb->end, mb->buf, mb->end);
	if (err) {
		warning("flowmgr(%p): send_log: http_request failed: %m\n",
			fm, err);
		goto out;
	}


 out:
	mem_deref(mb);
}


enum trace_type {
	TRACE_REQ  = 0,
	TRACE_RESP = 1,
	TRACE_WS   = 2
};


static void color_trace(enum trace_type type,
			int color, const char *fmt, ...)
{
#define WIDTH 128
	va_list ap;

	va_start(ap, fmt);

	char bufv[3][WIDTH] = {
		"----------------------------------------"
		"----------------------------------------"
		"----------------------------------------"
		"------>\n"
		,
		"<---------------------------------------"
		"----------------------------------------"
		"----------------------------------------"
		"-------\n"
		,
		"<- - - - - - - - - - - - - - - - - - - -"
		" - - - - - - - - - - - - - - - - - - - -"
		" - - - - - - - - - - - - - - - - - - - -"
		" - - - \n"
	};
	char *buf = bufv[type];
	char str[256];
	size_t len;
	size_t offset;

	re_snprintf(str, sizeof(str), "%v", fmt, &ap);
	len = min(WIDTH, strlen(str));

	offset = (WIDTH - len) / 2;

	memcpy(&buf[offset], str, len);

#ifndef ANDROID
	re_fprintf(stderr, "\x1b[%dm", color);
	write(STDERR_FILENO, buf, WIDTH);
	re_fprintf(stderr, "\x1b[;m");
#endif

	va_end(ap);
#undef WIDTH
}

static struct dnsc *dnsc = NULL;


/* This is just a dummy handler fo waking up re_main() */
static void wakeup_handler(int id, void *data, void *arg)
{
	(void)id;
	(void)data;
	(void)arg;
}


static int msystem_init(const char *msysname,
			const char *cert, size_t cert_len)
{
	int err;

	if (msys.inited)
		return 0;

	err = mqueue_alloc(&msys.mq, wakeup_handler, NULL);
	if (err) {
		warning("flowmgr: failed to create mqueue (%m)\n", err);
		return err;
	}

	err = tls_alloc(&msys.dtls, TLS_METHOD_DTLS, NULL, NULL);
	if (err) {
		warning("flowmgr: failed to create DTLS context (%m)\n",
			err);
		return err;
	}

	if (cert && cert_len) {
		info("flowmgr: set certificate (%zu bytes)\n", cert_len);

		err = tls_set_certificate(msys.dtls, cert, cert_len);
		if (err) {
			warning("flowmgr: failed to set certificate"
				" [%zu bytes] (%m)\n", cert_len, err);
			return err;
		}
	}
	else {
		info("flowmgr: generating selfsigned certificate..\n");

		err = tls_set_selfsigned(msys.dtls, "dtls@avs");
		if (err) {
			warning("flowmgr: failed to self-sign certificate"
				" (%m)\n", err);
			return err;
		}
	}

	tls_set_verify_client(msys.dtls);

	err = tls_set_srtp(msys.dtls, "SRTP_AES128_CM_SHA1_80");
	if (err) {
		warning("flowmgr: failed to enable SRTP profile (%m)\n",
			err);
		return err;
	}

	tmr_init(&msys.vol_tmr);

	err = str_dup(&msys.name, msysname);
	if (streq(msys.name, "audummy"))
		err = audummy_init(&aucodecl);
#if USE_MEDIAENGINE
	else if (streq(msys.name, "voe")) {
		err = voe_init(&aucodecl);
		if (err) {
			warning("flowmgr: voe init failed (%m)\n", err);
			return err;
		}
#if HAVE_VIDEO
		err = vie_init(&vidcodecl);
		if (err) {
			warning("flowmgr: vie init failed (%m)\n", err);
			return err;
		}
#endif

		msys.using_voe = true;
	}
#endif
	else {
		warning("flowmgr: media-system not available (%s)\n",
			msys.name);
		err = EINVAL;
	}

	if (err)
		return err;

	info("flowmgr: msystem successfully initialized\n");

	msys.inited = true;

	return 0;
}


void msystem_cancel_volume(void)
{
	if (msys.flows.head == NULL)
		tmr_cancel(&msys.vol_tmr);

}


struct tls *msystem_dtls(void)
{
	return msys.dtls;
}

struct list *msystem_aucodecl(void)
{
	return &aucodecl;
}

struct list *msystem_vidcodecl(void)
{
	return &vidcodecl;
}

struct list *msystem_flows(void)
{
	return &msys.flows;
}


bool msystem_get_dualstack(void)
{
	return msys.dualstack;
}


const char *msystem_get_interface(void)
{
	return msys.ifname;
}


#if USE_MEDIAENGINE
static void vol_tmr_handler(void *arg)
{
	struct le *le;
	bool using_voe = false;

	using_voe = msys.using_voe;

	LIST_FOREACH(&msys.flows, le) {
		struct flow *flow = le->data;

		flow_vol_handler(flow, using_voe);
	}

	if (msys.flows.head != NULL) {
		tmr_start(&msys.vol_tmr, VOLUME_TIMEOUT,
			  vol_tmr_handler, NULL);
	}
}

#if HAVE_VIDEO
static void flow_create_preview_handler(void *arg)
{
	struct flowmgr *fm = arg;

	if (fm->video.create_previewh) {
		fm->video.create_previewh(fm->video.arg);
	}
}


static void flow_release_preview_handler(void *view, void *arg)
{
	struct flowmgr *fm = arg;

	if (fm->video.release_previewh) {
		fm->video.release_previewh(view, fm->video.arg);
	}
}


static void flow_create_view_handler(const char *convid, const char *partid,
				     void *arg)
{
	struct flowmgr *fm = arg;

	if (fm->video.create_viewh) {
		fm->video.create_viewh(convid, partid, fm->video.arg);
	}
}


static void flow_release_view_handler(const char *convid, const char *partid,
				      void *view, void *arg)
{
	struct flowmgr *fm = arg;

	if (fm->video.release_viewh) {
		fm->video.release_viewh(convid, partid, view, fm->video.arg);
	}
}
#endif
#endif


void msystem_start_volume(void)
{
#if USE_MEDIAENGINE
	if (!tmr_isrunning(&msys.vol_tmr)) {
		tmr_start(&msys.vol_tmr, VOLUME_TIMEOUT,
			  vol_tmr_handler, NULL);
	}
#endif
}


static void msystem_free(void)
{
	if (!msys.inited)
		return;

	if (streq(msys.name, "audummy"))
		audummy_close();
#if USE_MEDIAENGINE
	else if (streq(msys.name, "voe")) {
#if HAVE_VIDEO
		vie_close();
#endif
		voe_close();
	}
#endif

	list_clear(&msys.flows);
	tmr_cancel(&msys.vol_tmr);

	msys.mq = mem_deref(msys.mq);
	msys.dtls = mem_deref(msys.dtls);
	msys.name = mem_deref(msys.name);
	
	msys.inited = false;
}


int flowmgr_init(const char *msysname, const char *log_url)
{
	int err;

	info("flowmgr_init: msys=%s\n", msysname);

	err = marshal_init();
	if (err) {
		error("flow-manager: failed to init marshalling\n");

		return err;
	}

	err = msystem_init(msysname, msys.cert, msys.cert_len);
	if (err)
		goto out;

	info("flow-manager initialized -- %s [machine %H]\n",
	     software, sys_build_get, 0);

	msys.log_url = mem_deref(msys.log_url);
	if (log_url) {
		err = str_dup(&msys.log_url, log_url);
		if (err)
			return err;
	}

 out:
	if (err)
		msystem_free();

	return err;
}


/* NOTE: must be called BEFORE flowmgr_init() */
void flowmgr_set_cert(const char *cert, size_t cert_len)
{
	msys.cert     = cert;
	msys.cert_len = cert_len;
}


static void config_handler(struct call_config *cfg, void *arg)
{
	struct flowmgr *fm = arg;
	struct le *le;

	(void)cfg;
	
	fm->config.pending = false;
	fm->config.ready = true;

	le = fm->postl.head;
	while(le) {
		struct call *call = le->data;
		
		le = le->next;

		call_postponed_flows(call);
		list_unlink(&call->post_le);
	}
}


int flowmgr_start(void)
{
	struct le *le;
	int err = 0;

	msys.started = true;
	
	LIST_FOREACH(&msys.flowmgrl, le) {
		struct flowmgr *fm = le->data;

		fm->config.pending = true;
		err |= flowmgr_config_starth(fm, config_handler, fm);
	}

	return err;
}


void flowmgr_enable_dualstack(bool enable)
{
	msys.dualstack = enable;
}


void flowmgr_bind_interface(const char *ifname)
{
	if (!ifname)
		return;

	info("flowmgr: binding to network interface '%s'\n", ifname);

	str_ncpy(msys.ifname, ifname, sizeof(msys.ifname));
}


struct call *flowmgr_call(struct flowmgr *fm, const char *convid)
{
	struct call *call;
	
	if (!fm)
		return NULL;

	call = dict_lookup(fm->calls, convid);

	return call;
}


void flowmgr_close(void)
{
	flowmgr_wakeup();
	marshal_close();
	msystem_free();
	dnsc = mem_deref(dnsc);
	msys.log_url = mem_deref(msys.log_url);
}


void flowmgr_network_changed(struct flowmgr *fm)
{
	info("flowmgr(%p): network_changed\n", fm);

	if (!fm)
		return;
       
	/* Go through all the calls, and restart flows on them */
	dict_apply(fm->calls, call_restart_handler, fm);
}


void flowmgr_mcat_changed(struct flowmgr *fm, const char *convid,
			  enum flowmgr_mcat mcat)
{
	struct call *call;

	debug("flowmgr(%p): mcat_changed: convid=%s mcat=%d\n",
	      fm, convid, mcat);

	call = dict_lookup(fm->calls, convid);

	call_mcat_changed(call, mcat);
}


static bool rr_exist(const struct flowmgr *fm, struct rr_resp *rr)
{
	struct le *le;

	if (!fm || !rr)
		return false;

	for (le = fm->rrl.head; le; le = le->next) {
		if (rr == le->data)
			return true;
	}

	return false;
}


int flowmgr_send_request(struct flowmgr *fm, struct call *call,
		         struct rr_resp *rr,
		         const char *path, const char *method,
		         const char *ctype, struct json_object *jobj)
{
	char *json = NULL;
	int err;

	if (!fm)
		return EINVAL;

	if (fm->trace) {
		color_trace(TRACE_REQ, 32, "%s %s", method, path);
	}
	if (fm->trace >= 2 && jobj) {

		re_fprintf(stderr, "\x1b[32m");
		jzon_dump(jobj);
		re_fprintf(stderr, "\x1b[;m");
	}
	
	if (jobj) {
		err = jzon_encode(&json, jobj);
		if (err) {
			warning("flowmgr: send_req:"
				" could not encode JSON-object (%p)\n", jobj);
			return err;
		}
	}

	debug("flowmgr(%p): send_request: %s %s content=%s\n",
	      fm, method, path, json);

	if (rr) {
		re_snprintf(rr->debug, sizeof(rr->debug),
			    "%s %s", method, path);
	}

	flowmgr_append_log(fm, call,
		"HTTP(%p) %s %s %s", rr, method, path, json);

	err = fm->reqh(rr, path, method,
		       ctype, json, str_len(json), fm->sarg);
	if (err) {
		warning("flowmgr: send_req: fm->reqh failed"
			" [%s %s %s %zu] (%m)",
			method, path, ctype, str_len(json), err
			);
	}
	mem_deref(json);
	return err;
}


int flowmgr_resp(struct flowmgr *fm, int status, const char *reason,
		 const char *ctype, const char *content, size_t clen,
		 struct rr_resp *rr)
{
	struct json_object *jobj = NULL;
	int err = 0;

	if (status >= 400) {
		warning("flowmgr_resp(%p): rr=%p status=%d content=%b\n",
			fm, rr, status, content, clen);
	}
	else {
		debug("flowmgr_resp(%p): rr=%p status=%d content=%b\n",
		      fm, rr, status, content, clen);
	}

	if (!fm)
		return EINVAL;
	
	if (!rr || !rr_exist(fm, rr)) {
	        if (rr)
			warning("flowmgr(%p): rr_resp %p does not exist\n",
				fm, rr);
			
		if (content && clen) {
			flowmgr_append_log(fm, NULL,
					   "HTTP(NORR) RESP %d %s %b",
					   status, reason, content, clen);
		}
		else {
			flowmgr_append_log(fm, NULL,
					   "HTTP(NORR) RESP %d %s",
					   status, reason);
		}

		return rr ? ENOENT : EINVAL;
	}

	if (content && clen) {
		flowmgr_append_log(fm, rr->call,
				   "HTTP(%p) RESP %d %s %b",
				   rr, status, reason, content, clen);
	}
	else {
		flowmgr_append_log(fm, rr->call,
				   "HTTP(%p) RESP %d %s",
				   rr, status, reason);
	}

	if (ctype && !streq(ctype, CTYPE_JSON)) {
		warning("flowmgr(%p): rest_resp: invalid content type: %s\n",
			fm, ctype);
		err = EPROTO;
		goto out;
	}

	if (ctype && content && clen > 0) {

		err = jzon_decode(&jobj, content, clen);
		if (err) {
			warning("flowmgr(%p): flowmgr_resp: JSON parse error"
				" [%zu bytes]\n",
				fm, clen);
			goto out;
		}
	}

	if (fm->trace >= 1) {

		if (status < 200 || status >= 300)
			color_trace(TRACE_RESP, 31, "%d %s", status, reason);
		else
			color_trace(TRACE_RESP, 32, "%d %s [%s]",
				    status, reason, ctype);
	}
	if (fm->trace >= 2 && content && clen) {
		re_fprintf(stderr, "\x1b[32m");
		jzon_dump(jobj);
		re_fprintf(stderr, "\x1b[;m");
	}

	if (rr)
		rr_response(rr);
	if (rr && rr->resph) {
		rr->resph(status, rr, jobj, rr->arg);
	}

 out:
	if (jobj) {
		json_object_put(jobj);
	}

	mem_deref(rr);

	return err;
}


static int flow_add(struct call *call, const char *flowid,
		    bool has_creator, bool is_creator,
		    const char *creator, struct json_object *jflow)
{
	struct flow *flow;
	const char *remote_user;
	bool active = false;
	int err;

	if (!msys.inited) {
		warning("flowmgr: mediasystem is not initialized!\n");
		return ENOSYS;
	}

	if (!call)
		return EINVAL;

	/* flow already exists? */
	flow = call_find_flow(call, flowid);
	if (flow)
		return 0;

	remote_user = jzon_str(jflow, "remote_user");
	if (!remote_user) {
		warning("flowmgr: flow_add: remote_user is missing\n");
	}
	if (has_creator) {
		if (creator == NULL)
			is_creator = true;
		else if (remote_user)
			is_creator = !streq(creator, remote_user);
	}

	jzon_bool(&active, jflow, "active");
	err = flow_alloc(&flow, call, flowid, remote_user,
			 is_creator, active);
	if (err) {
		warning("flowmgr(%p): flow_add: cannot allocate flow: %m\n",
			call_flowmgr(call), err);
		goto out;
	}

	flow_act_handler(flow, jflow);

#if USE_MEDIAENGINE && HAVE_VIDEO
	flow_set_video_handlers(
		flow,
		flow_create_preview_handler,
		flow_release_preview_handler,
		flow_create_view_handler,
		flow_release_view_handler,
		call_flowmgr(call));
#endif

 out:
	if (err)
		flow = mem_deref(flow);

	return err;
}


struct event {
	struct le le;
	char *ctype;
	struct mbuf *content;
};


static void event_replay(struct flowmgr *fm)
{
	struct le *le;
	struct event *ev;
	int max_num;

	if (!fm)
		return;

	max_num = list_count(&fm->eventq);

	info("flowmgr: event replay (count=%u)\n", list_count(&fm->eventq));

	le = fm->eventq.head;
	while (le && max_num) {

		ev = le->data;
		le = le->next;

		--max_num;

		debug("flowmgr: eventq pop: ctype=%s\n", ev->ctype);
		process_event(NULL, fm, ev->ctype,
			      (char*)ev->content->buf,
			      ev->content->end, true);

		mem_deref(ev);
	}
}


static int flows_add_list(struct flowmgr *fm, struct list *addl)
{
	struct le *le;
	int n = 0;
	int err = 0;

	LIST_FOREACH(addl, le) {
		struct flow_elem *flel = le->data;

		err = flow_add(flel->call, flel->flowid,
			       flel->has_creator, flel->is_creator,
			       flel->creator, flel->jflow);
		if (err)
			return err;
		++n;
	}

	if (n > 0)
		event_replay(fm);

	return err;
}


static int flows_del_list(struct list *dell)
{
	struct le *le;
	int err;

	LIST_FOREACH(dell, le) {
		struct flow_elem *flel = le->data;
		struct rr_resp *rr;

		err = rr_alloc(&rr, call_flowmgr(flel->call),
			       flel->call, call_ghost_flow_handler, flel);
		if (err)
			return err;

		flow_delete(flel->call, flel->flowid, rr, 0);
	}

	return 0;
}

static void flel_destructor(void *arg)
{
	struct flow_elem *flel = arg;

	mem_deref((void *)flel->creator);
}


static int add_flows(struct json_object *jobj, struct call *call,
		     bool is_creator)
{
	struct json_object *jflows;
	int n;
	int i, err = 0;
	struct list addl = LIST_INIT;
	bool has_creator = false;

	err = jzon_array(&jflows, jobj, "flows");
	if (err) {
		warning("flowmgr(%p): add_flows: no 'flows' array\n",
			call_flowmgr(call));
		return err;
	}

	n = json_object_array_length(jflows);

	/* get the first element in the 'flows' array */
	for (i = 0; i < n; ++i) {
		struct flow *flow;
		struct json_object *jflow;
		struct flow_elem *flel;
		const char *flowid;
		const char *sdp_step;
		const char *cid = NULL;
		char *creator = NULL;		
		bool active;

		jflow = json_object_array_get_idx(jflows, i);
		if (!jflow) {
			warning("flowmgr(%p): add_flow: flows[%d] missing\n",
				call_flowmgr(call), i);
			return ENOENT;
		}

		flowid = jzon_str(jflow, "id");

		/* Check if we have this flow already,
		 * i.e. added by call.flow-add
		 */
		flow = call_find_flow(call, flowid); 
		if (flow) {
			info("flowmgr(%p): add_flow: flow %s "
			     "already exists: flow=%p\n",
			     call_flowmgr(call), flowid, flow);
			continue;
		}

		sdp_step = jzon_str(jflow, "sdp_step");

#if 1
		err = jzon_bool(&active, jflow, "active");
		if (err) {
			warning("flowmgr(%p): add_flow: no active flag\n",
				call_flowmgr(call));
			return err;
		}
#endif

		/* The semantics of the creator attribute is:
		 * creator == NULL means that the POST has created the flow
		 * creator == user-id means that someone else has created
		 *
		 * Also keep in mind that the creator flag gives us info
		 * about where we are coming from i.e. flow-add event or
		 * POST for flows...
		 */
		err = jzon_is_null(jflow, "creator");
		if (err == 0)
			has_creator = true;
		else if (err == ENOENT)
			has_creator = false;
		else {
			has_creator = true;
			cid = jzon_str(jflow, "creator");
			if (cid)
				str_dup(&creator, cid);
			else
				creator = NULL;
		}
		
		flel = mem_zalloc(sizeof(*flel), flel_destructor);
		flel->call = call;
		flel->flowid = flowid;
		flel->has_creator = has_creator;
		flel->is_creator = is_creator;
		flel->creator = creator;
		flel->jflow = jflow;

		/* Check for ghost flows, defined as:
		 * sdp_step != "pending" and flow is not active
		 */
		if (sdp_step && !streq(sdp_step, "pending")) {
			if (active) {
				flel->is_creator = false;
				flel->has_creator = false;
				list_append(&addl, &flel->le, flel);
			}
			else {
				info("flowmgr(%p): ghost-flow with idx=%u"
				     " and sdp_step=%s -- deleting\n",
				     call_flowmgr(call), i, sdp_step);
				list_append(&call->ghostl, &flel->le, flel);
			}
		}
		else
			list_append(&addl, &flel->le, flel);
	}

	if (call->ghostl.head)
		flows_del_list(&call->ghostl);
	
	err = flows_add_list(call_flowmgr(call), &addl);
	list_flush(&addl);

	return err;
}


static void post_flows_resp(int status, struct rr_resp *rr,
			   struct json_object *jobj, void *arg)
{
	struct call *call = rr->call;
	struct flowmgr *fm = call_flowmgr(call);
	int err;	

	(void)arg;

	if (status < 200 || status >= 300) {
		warning("flowmgr(%p): post_flows_resp: failed: status = %d\n",
			fm, status);

		err = EPROTO;
		goto out;
	}

	err = add_flows(jobj, call, true);
	if (err) {
		warning("flowmgr(%p): add_flows() failed (%m)\n", fm, err);
	}

	info("flowmgr: post flows -- %u flows\n", dict_count(call->flows));

	call_purge_users(call);
	
 out:
	if (err) {
		if (fm && fm->errh)
			fm->errh(err, call_convid(call), fm->sarg);
	}
}


static int flows_add_handler(struct call *call, struct json_object *jobj)
{
	int err;

	err = add_flows(jobj, call, false);
	if (err)
		return err;

	info("flowmgr: add flows -- %u flows\n", dict_count(call->flows));

	event_replay(call_flowmgr(call));

	return err;
}


int flowmgr_post_flows(struct call *call)
{
	struct rr_resp *rr = NULL;
	struct flowmgr *fm = call_flowmgr(call);
	struct json_object *jsdp = NULL;
	char url[256];
	int err;

	if (!call)
		return EINVAL;

	/* Post for flows */
	err = rr_alloc(&rr, fm, call, post_flows_resp, NULL);
	if (err) {
		warning("flowmgr(%p): flowmgr_post_flows: "
			"cannot allocate rest response (%m)\n",
			fm, err);
		return err;
	}

	jsdp = call_userflow_sdp(call);

	re_snprintf(url, sizeof(url),
		    jsdp ? CREQ_POST_FLOWS : CREQ_FLOWS, call_convid(call));

	err = flowmgr_send_request(fm, call, rr, url, HTTP_POST,
				   jsdp ? CTYPE_JSON : NULL, jsdp);
	if (err) {
		warning("flowmgr(%p): flowmgr_post_flows: rest_req failed"
			" (%m)\n", fm, err);
		goto out;
	}

 out:
	if (jsdp)
		json_object_put(jsdp);
	
	if (err)
		mem_deref(rr);

	return err;
}


int flowmgr_acquire_flows(struct flowmgr *fm, const char *convid,
			  const char *sessid,
			  flowmgr_netq_h *qh, void *arg)
{
	struct call *call;
	bool callocated = false;
	int err;

	if (!fm || !convid)
		return EINVAL;

	debug("flowmgr(%p): acquire_flows: convid=%s\n", fm, convid);

	err = call_lookup_alloc(&call, &callocated, fm, convid);
	if (err)
		return err;

	call->start_ts = tmr_jiffies();

	flowmgr_append_log(fm, call,
			   "acquire_flows: convid=%s sessid=%s",
			   convid, sessid);

	if (sessid)
		call_set_sessid(call, sessid);

	call_set_active(call, true);

	if (fm->config.pending) {
		info("flowmgr(%p): %s: config pending wait with POST...\n",
		     fm, __func__);
		list_append(&fm->postl, &call->post_le, call);
		err = 0;
		goto out;
	}
	
	err = call_post_flows(call);
	if (err) {
		warning("flowmgr: %s: call_post failed for call:%p(%d)\n",
			__func__, call, err);
		goto out;
	}

 out:
	if (err) {
		if (callocated)
			mem_deref(call);
	}

	return err;
}


/* note: username is optional */
int flowmgr_user_add(struct flowmgr *fm, const char *convid,
		     const char *userid, const char *username)
{
	struct call *call;
	struct userflow *uf;
	bool call_alloc = false;
	bool uf_alloc = false;
	int err;

	if (!fm || !convid || !userid)
		return EINVAL;

	info("flowmgr(%p): user_add: convid=%s userid=%s\n",
	     fm, convid, userid);

	err = call_lookup_alloc(&call, &call_alloc, fm, convid);
	if (err) {
		warning("flowmgr: user_add: lookup (%m)\n", err);
		goto out;
	}

	err = call_userflow_lookup_alloc(&uf, &uf_alloc,
					 call, userid, username);
	if (err) {
		warning("flowmgr: user_add: userflow (%m)\n", err);
		goto out;
	}

 out:
	if (err) {
		if (call_alloc)
			mem_deref(call);
		if (uf_alloc)
			mem_deref(uf);
	}
	
	return err;
}


size_t flowmgr_users_count(const struct flowmgr *fm, const char *convid)
{
	struct call *call;

	if (!fm || !convid)
		return 0;

	call = dict_lookup(fm->calls, convid);
	if (!call)
		return 0;

	return dict_count(call->users);
}


void flowmgr_set_active(struct flowmgr *fm, const char *convid, bool active)
{
#if 0		
	struct call *call;
	
	if (!fm || !convid)
		return;
	
	debug("flowmgr(%p): set_active: convid=%s\n", fm, convid);

	call = dict_lookup(fm->calls, convid);

	if (call == NULL) {
		info("flowmgr(%p): flowmgr_set_active: no call on:"
		     " %s\n", fm, convid);
		return;
	}

	call_set_active(call, active);
#endif
}


void flowmgr_release_flows(struct flowmgr *fm, const char *convid)
{
	struct call *call;

	if (!fm)
		return;

	debug("flowmgr(%p): release_flows: convid=%s\n", fm, convid);

	call = dict_lookup(fm->calls, convid);

	flowmgr_append_log(fm, call,
			   "release_flows: convid=%s", convid);

	if (call == NULL) {
		info("flowmgr(%p): flowmgr_release_flows: no call on:"
		     " %s\n", fm, convid);
		return;
	}

	call_set_active(call, false);
	
	debug("flowmgr_release_flows(%p): metrics=%d logging=%d call=%p\n",
	      fm, fm->use_metrics, fm->use_logging, call);

	if (fm->use_metrics)
		flowmgr_send_metrics(fm, convid, "complete");

	call_log_codec_stats(call);

	call_cancel(call);

	if (fm->use_logging)
		send_log(fm, call);
	
	dict_remove(fm->calls, call_convid(call));
}


int flowmgr_sort_participants(struct list *partl)
{
	conf_pos_sort(partl);

	return 0;
}


static bool release_flows(char *key, void *val, void *arg)
{
	struct flowmgr *fm = arg;
	const char *convid = key;

	(void)val;

	flowmgr_release_flows(fm, convid);

	return false;
}


static void close_requests(struct flowmgr *fm)
{
	struct le *le = fm->rrl.head;

	while (le) {
		struct rr_resp *rr = le->data;
		le = le->next;

		if (rr->resph)
			rr->resph(499, rr, NULL, rr->arg);

		mem_deref(rr);
	}
}


static void fm_destructor(void *arg)
{
	struct flowmgr *fm = arg;

	debug("fm_destructor(%p)\n", fm);

	flowmgr_config_stop(fm);

	close_requests(fm);

	if (fm->calls)
		dict_apply(fm->calls, release_flows, fm);

	mem_deref(fm->calls);

	list_flush(&fm->logl);
	mem_deref(fm->httpc);
	list_flush(&fm->logreql);
	list_flush(&fm->eventq);
	list_flush(&fm->postl);

	list_unlink(&fm->le);
}


int flowmgr_alloc(struct flowmgr **fmp, flowmgr_req_h *reqh,
		  flowmgr_err_h *errh, void *arg)
{
	struct flowmgr *fm;
	int err;

	if (!fmp || !reqh) {
		return EINVAL;
	}

	info("flowmgr: alloc (%s)\n", avs_version_str());

	fm = mem_zalloc(sizeof(*fm), fm_destructor);
	if (!fm)
		return ENOMEM;

	err = dict_alloc(&fm->calls);
	if (err) {
		goto out;
	}

	fm->reqh = reqh;
	fm->errh = errh;
	fm->sarg = arg;

	/* Set these elsewhere, for more control... */
	flowmgr_enable_metrics(fm, true);
	flowmgr_enable_logging(fm, false);

	list_append(&msys.flowmgrl, &fm->le, fm);
	if (msys.started) {
		fm->config.pending = true;
		flowmgr_config_starth(fm, config_handler, fm);
	}

 out:
	if (err) {
		mem_deref(fm);
	}
	else {
		*fmp = fm;
	}

	return err;
}


int flowmgr_set_event_handler(struct flowmgr *fm, flowmgr_event_h *evh,
			      void *arg)
{
	if (!fm)
		return EINVAL;

	fm->evh = evh;
	fm->evarg = arg;

	return 0;
}


void flowmgr_set_media_handlers(struct flowmgr *fm, flowmgr_mcat_chg_h *cath,
			        flowmgr_volume_h *volh, void *arg)
{
	if (!fm)
		return;

	fm->cath = cath;
	fm->volh = volh;
	fm->marg = arg;
}


void flowmgr_set_media_estab_handler(struct flowmgr *fm,
				     flowmgr_media_estab_h *mestabh,
				     void *arg)
{
	if (!fm)
		return;

	fm->mestabh = mestabh;
	fm->mestab_arg = arg;
}


void flowmgr_set_log_handlers(struct flowmgr *fm,
			      flowmgr_log_append_h *appendh,
			      flowmgr_log_upload_h *uploadh,
			      void *arg)
{
	if (!fm)
		return;

	fm->log.appendh = appendh;
	fm->log.uploadh = uploadh;
	fm->log.arg = arg;
}


void flowmgr_set_conf_pos_handler(struct flowmgr *fm,
				  flowmgr_conf_pos_h *conf_posh,
				  void *arg)
{
	if (!fm)
		return;

	fm->conf_posh = conf_posh;
	fm->conf_pos_arg = arg;
}


void flowmgr_set_video_handlers(struct flowmgr *fm, 
				flowmgr_create_preview_h *create_previewh,
				flowmgr_release_preview_h *release_previewh,
				flowmgr_create_view_h *create_viewh,
				flowmgr_release_view_h *release_viewh,
				void *arg)
{
	fm->video.create_previewh = create_previewh;
	fm->video.release_previewh = release_previewh;
	fm->video.create_viewh = create_viewh;
	fm->video.release_viewh = release_viewh;
	fm->video.arg = arg;
}


void flowmgr_set_sessid(struct flowmgr *fm,
			const char *convid, const char *sessid)
{
	struct call *call;
	bool created = false;
	int err = 0;

	if (!fm || !convid)
		return;

	debug("flowmgr(%p): set_sessid: convid=%s sessid=%s\n",
	     fm, convid, sessid);
    
	call = dict_lookup(fm->calls, convid);
	if (!call) {
		if (!call) {
			err = call_alloc(&call, fm, convid, NULL, NULL);
			if (err)
				goto out;

			created = true;
		}
	}

	flowmgr_append_log(fm, call,
			   "set_sessid: convid=%s sessid=%s",
			   convid, sessid);

	err = call_set_sessid(call, sessid);
    
 out:
	if (err) {
		if (created)
			mem_deref(call);
	}
}


int  flowmgr_interruption(struct flowmgr *fm, const char *convid,
			  bool interrupted)
{
	struct call *call;
	
	if (!fm)
		return EINVAL;

	call = dict_lookup(fm->calls, convid);
	if (!call)
		return ENOENT;

	call_interruption(call, interrupted);
	
	return 0;
}


void flowmgr_background(struct flowmgr *fm, enum media_bg_state bgst)
{
	dict_apply(fm->calls, call_background_handler, (void *)bgst);

	// Also needed to handle the preview
	vie_preview_background(bgst, fm->video.create_previewh, fm->video.release_previewh,
		fm->video.arg);
}


void flowmgr_enable_trace(struct flowmgr *fm, int trace)
{
	if (!fm)
		return;

	fm->trace = trace;
}


void flowmgr_enable_metrics(struct flowmgr *fm, bool metrics)
{
	if (!fm)
		return;

	fm->use_metrics = metrics;
}


void flowmgr_enable_logging(struct flowmgr *fm, bool logging)
{
	if (!fm)
		return;

	fm->use_logging = logging;
	if (logging && !fm->httpc) {
		int err;

		if (!dnsc) {
			err = dns_init(&dnsc);
			if (err) {
				warning("flowmgr(%p): dns_init failed (%m)\n",
					fm, err);
				goto out;
			}
		}
		err = http_client_alloc(&fm->httpc, dnsc);
		if (err) {
			error("flowmgr_enable_logging: "
			      "HTTP client failed: %m\n");
			goto out;
		}
	}
	fm->use_logging = logging;
	return;

 out:
	fm->use_logging = false;
}


struct flowmgr *flowmgr_free(struct flowmgr *fm)
{
	return mem_deref(fm);
}


static void event_destructor(void *data)
{
	struct event *ev = data;

	list_unlink(&ev->le);
	mem_deref(ev->ctype);
	mem_deref(ev->content);
}


static void event_enqueue(struct flowmgr *fm,
			  const char *ctype, const char *content, size_t clen)
{
	struct event *ev;

	ev = mem_zalloc(sizeof(*ev), event_destructor);

	str_dup(&ev->ctype, ctype);
	ev->content = mbuf_alloc(clen);
	mbuf_write_mem(ev->content, (void *)content, clen);

	list_append(&fm->eventq, &ev->le, ev);

	return;
}

int flowmgr_process_event(bool *hp, struct flowmgr *fm,
			  const char *ctype, const char *content, size_t clen)
{
	return process_event(hp, fm, ctype, content, clen, false);
}


static int process_event(bool *hp, struct flowmgr *fm,
			 const char *ctype, const char *content, size_t clen,
			 bool replayed)
{
	struct json_object *jobj = NULL;
	const char *ev;
	const char *convid;
	const char *flowid;
	struct call *call = NULL;
	struct flow *flow = NULL;
	bool handled = true;
	bool created = false;
	enum flowmgr_event event;
	int err = 0;

	if (!fm || !ctype)
		return EINVAL;

	if (!streq(ctype, CTYPE_JSON)) {
		warning("flowmgr: process_event: ctype %s\n", ctype);
		return EPROTO;
	}

	debug("flowmgr(%p): process_event: content=%b\n", fm, content, clen);

	err = jzon_decode(&jobj, content, clen);
	if (err) {
		warning("flowmgr(%p): process_event: JSON parse error"
			" [%zu bytes]\n", fm, clen);
		goto out;
	}

	ev = jzon_str(jobj, "type");

	convid = jzon_str(jobj, "conversation");
	flowid = jzon_str(jobj, "flow");

	if (!convid) {
		err = EPROTO;
		goto out;
	}

	/* check if we have a call for this conversation */
	call = dict_lookup(fm->calls, convid);

	flowmgr_append_log(fm, call, "EVENT(%zu bytes) %b",
			   clen, content, clen);

	if (streq(ev, events[FLOWMGR_EVENT_FLOW_ADD]))
		event = FLOWMGR_EVENT_FLOW_ADD;
	else if (streq(ev, events[FLOWMGR_EVENT_FLOW_DEL]))
		event = FLOWMGR_EVENT_FLOW_DEL;
	else if (streq(ev, events[FLOWMGR_EVENT_FLOW_ACT]))
		event = FLOWMGR_EVENT_FLOW_ACT;
	else if (streq(ev, events[FLOWMGR_EVENT_CAND_ADD]))
		event = FLOWMGR_EVENT_CAND_ADD;
	else if (streq(ev, events[FLOWMGR_EVENT_CAND_UPD]))
		event = FLOWMGR_EVENT_CAND_UPD;
	else if (streq(ev, events[FLOWMGR_EVENT_SDP]))
		event = FLOWMGR_EVENT_SDP;
	else {
		handled = false;
		goto out;
	}

	if (call && flowid) {
		flow = call_find_flow(call, flowid);

		if (!flow) {
			if (event == FLOWMGR_EVENT_FLOW_DEL) {
				info("flowmgr(%p): process_event: flow '%s'"
				     " already deleted\n", fm, flowid);
			}
			else {
				info("flowmgr(%p): process_event(%s): "
				     "cannot find flow '%s'"
				     " in [%u entries] -- queing..\n",
				     fm, ev, flowid,
				     call_count_flows(call));

				event_enqueue(fm, ctype, content, clen);

				err = 0;
				goto out;
			}
			err = EPROTO;
			goto out;
		}
	}

	if (handled) {

		if (fm->evh) {
			fm->evh(event, convid, flowid, jobj, fm->evarg);
		}

		if (fm->trace) {
			color_trace(TRACE_WS, 33, "%s", ev);
		}
		if (fm->trace >= 2) {
			re_fprintf(stderr, "\x1b[33m");
			jzon_dump(jobj);
			re_fprintf(stderr, "\x1b[;m");
		}
	}

	switch (event) {

	case FLOWMGR_EVENT_FLOW_ADD:
		if (!call) {
			err = call_alloc(&call, fm, convid, NULL, NULL);
			if (err)
				goto out;
			
			created = true;
		}
		err = flows_add_handler(call, jobj);
		break;

	case FLOWMGR_EVENT_FLOW_DEL:
		err = flow_del_handler(flow);
		if (call && dict_count(call->flows) == 0)
			dict_remove(fm->calls, convid);	
		break;

	case FLOWMGR_EVENT_FLOW_ACT:
		err = flow_act_handler(flow, jobj);
		break;

	case FLOWMGR_EVENT_CAND_ADD:
	case FLOWMGR_EVENT_CAND_UPD:
		err = flow_cand_handler(flow, jobj);
		break;

	case FLOWMGR_EVENT_SDP:
		err = flow_sdp_handler(flow, jobj, replayed);
		break;

	default:
		info("event ignored (%s)\n", ev);
		break;
	}

 out:
	if (jobj) {
		json_object_put(jobj);
	}

	if (err) {
		if (created)
			mem_deref(call);
	}
	else {
		if (hp)
			*hp = handled;
	}

	return err;
}


int flowmgr_has_active(struct flowmgr *fm, bool *has_active)
{
	struct call *call;

	if (!fm || !has_active)
		return EINVAL;

	call = dict_apply(fm->calls, call_active_handler, NULL);

	*has_active = call != NULL;

	return 0;
}


int flowmgr_has_media(struct flowmgr *fm, const char *convid,
		      bool *has_media)
{
	struct call *call;

	if (!fm || !has_media)
		return EINVAL;

	call = dict_lookup(fm->calls, convid);

	*has_media = call ? call->rtp_started : false;

	return 0;
	
}




struct flowmgr *flowmgr_rr_flowmgr(const struct rr_resp *rr)
{
	return rr ? rr->fm : NULL;
}


const char **flowmgr_events(int *nevs)
{
	if (nevs == NULL)
		return NULL;

	*nevs = FLOWMGR_EVENT_MAX;

	return events;
}


int flowmgr_send_metrics(struct flowmgr *fm, const char *convid,
			 const char *path)
{
	struct call *call;
	struct json_object *jobj;
	char url[256];
	bool handled;
	int err = 0;

	if (!fm || !convid)
		return EINVAL;

	call = dict_lookup(fm->calls, convid);
	if (!call)
		return ENOENT;

	jobj = json_object_new_object();
	if (!jobj)
		return ENOMEM;

	json_object_object_add(jobj, "version",
			       json_object_new_string(software));	
	handled = call_stats_prepare(call, jobj);

#if 0
	jzon_dump(jobj);
#endif
	if (!path) 
		re_snprintf(url, sizeof(url), CREQ_METRICS, call_convid(call));
	else {
		re_snprintf(url, sizeof(url), CREQ_METRICS"/%s",
			    call_convid(call), path);
	}

	err = flowmgr_send_request(fm, call, NULL,
				   url, HTTP_POST, CTYPE_JSON, jobj);
	if (err) {
		warning("flowmgr(%p): send_metrics: rest_req failed (%m)\n",
			fm, err);
		goto out;
	}

 out:
	json_object_put(jobj);

	return err;
}


int flowmgr_debug(struct re_printf *pf, const struct flowmgr *fm)
{
	const struct call_config *cfg;
	unsigned i;
	int err = 0;

	if (!fm)
		return 0;

	err |= re_hprintf(pf, "***** FLOWMGR *****\n");

	if (str_isset(fm->self_userid))
		err |= re_hprintf(pf, "self_userid: %s\n", fm->self_userid);

	cfg = &fm->config.cfg;

	err |= re_hprintf(pf, "iceservers: (%zu)\n", cfg->iceserverc);
	for (i=0; i<cfg->iceserverc; i++) {
		const struct zapi_ice_server *srv = &cfg->iceserverv[i];
		err |= re_hprintf(pf, "  %u: url=%s username=%s\n",
				  i, srv->url, srv->username);
	}

	err |= re_hprintf(pf, "\n");

	err |= re_hprintf(pf, "number of calls: %u\n", dict_count(fm->calls));
	dict_apply(fm->calls, call_debug_handler, pf);

	err |= re_hprintf(pf, "***** ******* *****\n");

	return err;
}


int flowmgr_wakeup(void)
{
	if (!msys.mq)
		return ENOSYS;

	return mqueue_push(msys.mq, 0, NULL);
}

#if HAVE_VIDEO

bool flowmgr_can_send_video(struct flowmgr *fm, const char *convid)
{
	struct call *call;

	if (convid) {
		call = dict_lookup(fm->calls, convid);
	}
	else {
		call = dict_apply(fm->calls, call_active_handler, NULL);
	}

	if (!call) {
		warning("flowmgr(%p): can_send_video: conv %s not found\n",
			fm, convid ? convid : "NULL");
		return false;
	}

	return call_can_send_video(call);
}

void flowmgr_set_video_send_state(struct flowmgr *fm, const char *convid, enum flowmgr_video_send_state state)
{
	struct call *call;

	if (!fm)
		return;
	
	switch (state) {
		case FLOWMGR_VIDEO_PREVIEW:
			vie_activate_video_preview(fm->video.create_previewh, fm->video.arg);
			break;

		case FLOWMGR_VIDEO_SEND_NONE:
			vie_deactivate_video_preview(fm->video.release_previewh, fm->video.arg);
			break;

		case FLOWMGR_VIDEO_SEND:
			call = convid ?
				dict_lookup(fm->calls, convid) :
				dict_apply(fm->calls, call_active_handler, NULL);

			if (call) {
				call_set_video_send_active(call, true);
			}
			break;
	}

	if (state != FLOWMGR_VIDEO_SEND) {
		// If UI call set_state preview or none, stop sending on active conv
		// since that is not always the convid given
		call = dict_apply(fm->calls, call_active_handler, NULL);
		if (convid && !call) {
			call = dict_lookup(fm->calls, convid);
		}
		if (call) {
			call_set_video_send_active(call, false);
		}
	}
}


bool flowmgr_is_sending_video(struct flowmgr *fm,
			      const char *convid, const char *partid)
{
	struct call *call;

	if (!fm)
		return false;

	if (convid) {
		call = dict_lookup(fm->calls, convid);
	}
	else {
		call = dict_apply(fm->calls, call_active_handler, NULL);
	}

	if (!call) {
		warning("flowmgr(%p): is_sending_video: conv %s not found\n",
			fm, convid ? convid : "NULL");
		return false;
	}

	return call_is_sending_video(call, partid);
}


void flowmgr_set_video_preview(struct flowmgr *fm, const char *convid, void *view)
{
	struct call *call;

	if (convid) {
		call = dict_lookup(fm->calls, convid);
	}
	else {
		call = dict_apply(fm->calls, call_active_handler, NULL);
	}

	if (!call || call_has_media(call) == false) {
		vie_set_preview(NULL, view);
		return;
	}

	call_set_video_preview(call, view);
}

void flowmgr_set_video_view(struct flowmgr *fm, const char *convid, const char *partid, void *view)
{
	struct call *call;

	if (convid) {
		call = dict_lookup(fm->calls, convid);
	}
	else {
		call = dict_apply(fm->calls, call_active_handler, NULL);
	}

	if (!call) {
		warning("flowmgr(%p): set_video_view: conv %s not found\n",
			fm, convid);
		return;
	}

	call_set_video_view(call, partid, view);
}

void flowmgr_get_video_capture_devices(struct flowmgr *fm, struct list **device_list)
{
#if USE_MEDIAENGINE
	vie_get_video_capture_devices(device_list);
#endif
}
void flowmgr_set_video_capture_device(struct flowmgr *fm, const char *convid, const char *devid)
{
	struct call *call;

	if (convid) {
		call = dict_lookup(fm->calls, convid);
	}
	else {
		call = dict_apply(fm->calls, call_active_handler, NULL);
	}

	if (!call) {
		warning("flowmgr(%p): set_video_capture_device: "
			"conv %s not found\n", fm, convid);
		return;
	}

	call_set_video_capture_device(call, devid);
}

#endif


bool flowmgr_is_using_voe(void)
{
	return msys.using_voe;
}


void flowmgr_set_username_handler(struct flowmgr *fm,
				  flowmgr_username_h *usernameh, void *arg)
{
	if (!fm)
		return;

	fm->username.nameh = usernameh;
	fm->username.arg = arg;
}


int flowmgr_is_ready(struct flowmgr *fm, bool *is_ready)
{
	if (!fm || !is_ready)
		return EINVAL;

	*is_ready = fm->config.ready;

	return 0;
}


const char *flowmgr_get_username(struct flowmgr *fm, const char *userid)
{
	if (!fm->username.nameh)
		return NULL;
	
	return fm->username.nameh(userid, fm->username.arg);
}


void flowmgr_set_self_userid(struct flowmgr *fm, const char *userid)
{
	if (!fm)
		return;

	info("flowmgr(%p): setting self_userid to: %s\n", fm, userid);
	
	str_ncpy(fm->self_userid, userid, sizeof(fm->self_userid));
}


const char *flowmgr_get_self_userid(struct flowmgr *fm)
{
	return fm ? fm->self_userid : NULL;
}
	

void flowmgr_refresh_access_token(struct flowmgr *fm,
				  const char *token, const char *type)
{
	if (!fm)
		return;

	info("flowmgr(%p): access_token: %s %s\n",
	     fm, token, type);

	str_ncpy(fm->rest.token.access_token, token,
		 sizeof(fm->rest.token.access_token));
	str_ncpy(fm->rest.token.token_type, type,
		 sizeof(fm->rest.token.token_type));
	
	if (fm->rest.cli) {
		rest_client_set_token(fm->rest.cli, &fm->rest.token);
	}

	fm->config.pending = true;
	flowmgr_config_starth(fm, config_handler, fm);
}
