#include "tasks.hpp"
#include "protocol.pb.h"

void keepalive_task(Connector &conn) {
    Request req = Request_init_zero;
    req.which_command = Request_ping_tag;
    req.command.ping.dummy_field = true;
    conn.send(req);
}
