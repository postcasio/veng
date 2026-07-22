#include <iostream>

#include <Veng/Veng.h>

#include "Cli.h"

// The vengtrace CLI: a thin front end over RunVengtraceCli, which carries the whole tool so it can be
// driven in-process by the tests. The executable only marshals argv and wires the standard streams.
int main(int argc, char** argv)
{
    const Veng::vector<Veng::string> args(argv + 1, argv + argc);
    return Veng::VengTrace::RunVengtraceCli(args, std::cout, std::cerr);
}
