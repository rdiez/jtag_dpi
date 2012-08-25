
#define __STDC_LIMIT_MACROS

#include "Vminsoc_bench_core.h"

#include <stdint.h>
#include <limits.h>


static uint64_t current_simulation_time = 0;

double sc_time_stamp ()
{
  return double( current_simulation_time );
} 


int main ( int argc, char **argv, char **env )
{
  Verilated::commandArgs(argc, argv);  // Remember args for $value$plusargs() and the like.
  Verilated::debug(0);  // Comment from Verilator example: "We compiled with it on for testing, turn it back off"

  Vminsoc_bench_core * const top = new Vminsoc_bench_core;

  // TODO: The reset level can be positive or negative, we need a parameter here.
  //       At the moment, the reset level is negative (0 means reset, 1 means no reset).
  
  top->reset = 0;

  while (!Verilated::gotFinish())
  {
    if (current_simulation_time > 20)
    {
      top->reset = 1;  // Deassert reset
    }

    top->clock = !top->clock;
    
    top->eval();
    
    ++current_simulation_time;

    // Provide an early warning against the remote possibility of a wrap-around.
    assert( current_simulation_time < UINT64_MAX / 100000 );
  }
  
  top->final();

  return 0;
}
