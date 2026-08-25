#ifndef LLAMA_SERVER_H
#define LLAMA_SERVER_H

#include "server-route-extensions.h"

struct common_params;

int llama_server(int argc, char ** argv);
int llama_server(int argc, char ** argv, const server_responses_routes_factory & responses_factory);
int llama_server(int argc, char ** argv, const server_route_extensions & extensions);

// Used by the llama CLI, where argument parsing and backend initialization have
// already happened.
int llama_server(common_params & params, int argc, char ** argv);
int llama_server(
        common_params & params,
        int argc,
        char ** argv,
        const server_responses_routes_factory & responses_factory);
int llama_server(
        common_params & params,
        int argc,
        char ** argv,
        const server_route_extensions & extensions);

// Installs the process-wide factory used by both stock llama_server() entry
// points. Call this before starting a server. Passing an empty factory restores
// the legacy in-process Responses implementation. This hook allows a statically
// linked Responses module to activate in the normal llama-server executable.
void llama_server_set_responses_routes_factory(server_responses_routes_factory responses_factory);

// Installs all compatibility decorators used by the stock server entry points.
// Passing an empty bundle restores the upstream route implementations.
void llama_server_set_route_extensions(server_route_extensions extensions);

void llama_server_terminate();

#endif  // LLAMA_SERVER_H
