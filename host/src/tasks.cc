#include "tasks.hpp"
#include "protocol.pb.h"

void keepalive_task(Connector &conn) {
    Request req = Request_init_zero;
    req.which_command = Request_ping_tag;
    conn.send(req);
}
