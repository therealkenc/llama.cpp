#include "server-integration.h"
#include "server.h"

int main(int argc, char ** argv) {
    return llama_server(argc, argv, llama_responses::make_server_responses_routes_factory());
}
