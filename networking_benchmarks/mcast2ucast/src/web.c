#include "web.h"
#include "stats.h"
#include "group_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdatomic.h>

#define WEB_BACKLOG    8
#define WEB_BUF_SIZE   512
#define WEB_RESP_SIZE  16384

struct dp_stats g_stats;

static int             server_fd = -1;
static pthread_t       web_thread;
static volatile int    web_running;

/* ---- JSON builder for /api/stats ---- */

struct json_ctx {
	char *buf;
	int   off;
	int   cap;
	int   first_group;
};

static void
json_append(struct json_ctx *j, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(j->buf + j->off, j->cap - j->off, fmt, ap);
	va_end(ap);
	if (n > 0 && j->off + n < j->cap)
		j->off += n;
}

static int
json_group_cb(uint32_t group_ip, const struct subscriber_list *subs, void *ctx)
{
	struct json_ctx *j = ctx;
	char grp[INET_ADDRSTRLEN], sub[INET_ADDRSTRLEN];
	struct in_addr a = { .s_addr = group_ip };

	inet_ntop(AF_INET, &a, grp, sizeof(grp));

	if (!j->first_group)
		json_append(j, ",");
	j->first_group = 0;

	json_append(j, "{\"group\":\"%s\",\"subscribers\":[", grp);

	for (uint32_t i = 0; i < subs->count; i++) {
		a.s_addr = subs->entries[i].unicast_ip;
		inet_ntop(AF_INET, &a, sub, sizeof(sub));
		if (i > 0)
			json_append(j, ",");
		json_append(j,
			"{\"ip\":\"%s\",\"port\":%u,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
			sub,
			ntohs(subs->entries[i].unicast_port),
			subs->entries[i].dst_mac.addr_bytes[0],
			subs->entries[i].dst_mac.addr_bytes[1],
			subs->entries[i].dst_mac.addr_bytes[2],
			subs->entries[i].dst_mac.addr_bytes[3],
			subs->entries[i].dst_mac.addr_bytes[4],
			subs->entries[i].dst_mac.addr_bytes[5]);
	}

	json_append(j, "]}");
	return 0;
}

static int
build_stats_json(char *buf, int cap)
{
	struct json_ctx j = { .buf = buf, .cap = cap, .off = 0, .first_group = 1 };
	time_t now = time(NULL);
	uint64_t uptime = (uint64_t)(now - g_stats.start_time);

	json_append(&j,
		"{\"uptime\":%lu,"
		"\"rx\":%lu,\"tx\":%lu,\"mcast\":%lu,\"drop\":%lu,\"igmp\":%lu,"
		"\"groups\":[",
		uptime,
		atomic_load(&g_stats.rx_packets),
		atomic_load(&g_stats.tx_packets),
		atomic_load(&g_stats.mcast_packets),
		atomic_load(&g_stats.drop_packets),
		atomic_load(&g_stats.igmp_packets));

	group_table_iterate(json_group_cb, &j);

	json_append(&j, "]}");
	return j.off;
}

/* ---- Embedded HTML dashboard ---- */

static const char dashboard_html[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>mcast2ucast dashboard</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,monospace;\n"
"  background:#0f1117;color:#c9d1d9;padding:24px}\n"
"h1{font-size:22px;color:#58a6ff;margin-bottom:8px}\n"
".meta{font-size:13px;color:#8b949e;margin-bottom:24px}\n"
".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-bottom:24px}\n"
".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px}\n"
".card .label{font-size:11px;text-transform:uppercase;color:#8b949e;letter-spacing:.5px}\n"
".card .value{font-size:28px;font-weight:700;color:#f0f6fc;margin-top:4px}\n"
".card .rate{font-size:12px;color:#8b949e;margin-top:2px}\n"
".card.rx .value{color:#58a6ff}\n"
".card.tx .value{color:#3fb950}\n"
".card.mcast .value{color:#d2a8ff}\n"
".card.drop .value{color:#f85149}\n"
".card.igmp .value{color:#f0883e}\n"
"h2{font-size:16px;color:#c9d1d9;margin-bottom:12px}\n"
"table{width:100%;border-collapse:collapse;background:#161b22;border:1px solid #30363d;border-radius:8px;overflow:hidden}\n"
"th{background:#21262d;text-align:left;padding:10px 14px;font-size:12px;text-transform:uppercase;color:#8b949e;letter-spacing:.5px}\n"
"td{padding:10px 14px;border-top:1px solid #30363d;font-size:14px}\n"
"tr:hover td{background:#1c2128}\n"
".dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#3fb950;margin-right:8px}\n"
".mono{font-family:monospace;font-size:13px;color:#8b949e}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<h1>mcast2ucast</h1>\n"
"<div class=\"meta\">uptime: <span id=\"uptime\">-</span> | refreshing every 1s</div>\n"
"<div class=\"cards\">\n"
"  <div class=\"card rx\"><div class=\"label\">RX Packets</div><div class=\"value\" id=\"rx\">0</div><div class=\"rate\" id=\"rx_rate\"></div></div>\n"
"  <div class=\"card tx\"><div class=\"label\">TX Packets</div><div class=\"value\" id=\"tx\">0</div><div class=\"rate\" id=\"tx_rate\"></div></div>\n"
"  <div class=\"card mcast\"><div class=\"label\">Multicast</div><div class=\"value\" id=\"mcast\">0</div><div class=\"rate\" id=\"mcast_rate\"></div></div>\n"
"  <div class=\"card drop\"><div class=\"label\">Dropped</div><div class=\"value\" id=\"drop\">0</div><div class=\"rate\" id=\"drop_rate\"></div></div>\n"
"  <div class=\"card igmp\"><div class=\"label\">IGMP</div><div class=\"value\" id=\"igmp\">0</div></div>\n"
"</div>\n"
"<h2>Multicast Groups</h2>\n"
"<table>\n"
"<thead><tr><th>Group</th><th>Subscribers</th><th>Destinations</th></tr></thead>\n"
"<tbody id=\"groups\"><tr><td colspan=\"3\" style=\"color:#8b949e\">Loading...</td></tr></tbody>\n"
"</table>\n"
"<script>\n"
"let prev={rx:0,tx:0,mcast:0,drop:0,t:Date.now()};\n"
"function fmt(n){if(n>=1e9)return(n/1e9).toFixed(1)+'B';if(n>=1e6)return(n/1e6).toFixed(1)+'M';if(n>=1e3)return(n/1e3).toFixed(1)+'K';return n}\n"
"function upfmt(s){let d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),sec=s%60;return(d?d+'d ':'')+(h?h+'h ':'')+(m?m+'m ':'')+sec+'s'}\n"
"async function poll(){\n"
"  try{\n"
"    let r=await fetch('/api/stats');\n"
"    let d=await r.json();\n"
"    let now=Date.now(),dt=(now-prev.t)/1000;\n"
"    document.getElementById('uptime').textContent=upfmt(d.uptime);\n"
"    ['rx','tx','mcast','drop'].forEach(k=>{\n"
"      document.getElementById(k).textContent=fmt(d[k]);\n"
"      let el=document.getElementById(k+'_rate');\n"
"      if(el&&dt>0){let rate=Math.round((d[k]-prev[k])/dt);el.textContent=rate>0?fmt(rate)+'/s':''}\n"
"    });\n"
"    document.getElementById('igmp').textContent=fmt(d.igmp);\n"
"    prev={rx:d.rx,tx:d.tx,mcast:d.mcast,drop:d.drop,t:now};\n"
"    let tb=document.getElementById('groups');\n"
"    if(d.groups.length===0){tb.innerHTML='<tr><td colspan=\"3\" style=\"color:#8b949e\">No active groups</td></tr>';}\n"
"    else{tb.innerHTML=d.groups.map(g=>'<tr><td><span class=\"dot\"></span>'+g.group+'</td><td>'+g.subscribers.length+'</td><td>'+g.subscribers.map(s=>'<span class=\"mono\">'+s.ip+':'+s.port+'</span>').join('<br>')+'</td></tr>').join('');}\n"
"  }catch(e){}\n"
"}\n"
"setInterval(poll,1000);poll();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ---- HTTP handling ---- */

static void
send_response(int fd, const char *status, const char *content_type,
	      const char *body, int body_len)
{
	char hdr[256];
	int hdr_len = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n",
		status, content_type, body_len);
	write(fd, hdr, hdr_len);
	if (body_len > 0)
		write(fd, body, body_len);
}

static void
handle_client(int fd)
{
	char req[WEB_BUF_SIZE];
	ssize_t n = read(fd, req, sizeof(req) - 1);
	if (n <= 0) {
		close(fd);
		return;
	}
	req[n] = '\0';

	if (strncmp(req, "GET /api/stats", 14) == 0) {
		char json[WEB_RESP_SIZE];
		int len = build_stats_json(json, sizeof(json));
		send_response(fd, "200 OK", "application/json", json, len);
	} else if (strncmp(req, "GET / ", 6) == 0 ||
		   strncmp(req, "GET /index", 10) == 0) {
		send_response(fd, "200 OK", "text/html; charset=utf-8",
			      dashboard_html, (int)sizeof(dashboard_html) - 1);
	} else {
		const char *body = "404 Not Found\n";
		send_response(fd, "404 Not Found", "text/plain",
			      body, (int)strlen(body));
	}

	close(fd);
}

static void *
web_thread_fn(void *arg)
{
	(void)arg;
	while (web_running) {
		struct sockaddr_in cli;
		socklen_t cli_len = sizeof(cli);

		/* Use a timeout so we can check web_running periodically */
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		int client = accept(server_fd, (struct sockaddr *)&cli, &cli_len);
		if (client < 0)
			continue;
		handle_client(client);
	}
	return NULL;
}

int
web_start(uint16_t port)
{
	struct sockaddr_in addr;
	int opt = 1;

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0)
		return -1;

	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(server_fd);
		server_fd = -1;
		return -1;
	}

	if (listen(server_fd, WEB_BACKLOG) < 0) {
		close(server_fd);
		server_fd = -1;
		return -1;
	}

	web_running = 1;
	if (pthread_create(&web_thread, NULL, web_thread_fn, NULL) != 0) {
		close(server_fd);
		server_fd = -1;
		return -1;
	}

	return 0;
}

void
web_stop(void)
{
	if (!web_running)
		return;
	web_running = 0;
	pthread_join(web_thread, NULL);
	if (server_fd >= 0) {
		close(server_fd);
		server_fd = -1;
	}
}
