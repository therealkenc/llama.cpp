#ifndef LLAMA_SERVER_H
#define LLAMA_SERVER_H

#include "server-route-extensions.h"

struct common_params;

int llama_server(int argc, char ** argv);
int llama_server(int argc, char ** argv, const server_route_extensions & extensions);

// Used by the llama CLI, where argument parsing and backend initialization have
// already happened.
int llama_server(common_params & params, int argc, char ** argv);
int llama_server(common_params & params, int argc, char ** argv, const server_route_extensions & extensions);

void llama_server_terminate();

#endif  // LLAMA_SERVER_H
