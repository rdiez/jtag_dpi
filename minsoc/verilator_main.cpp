
// Copyright (c) 2012, R. Diez

#define __STDC_LIMIT_MACROS
#define __STDC_FORMAT_MACROS  // For PRIu64

#include <stdint.h>
#include <signal.h>
#include <limits.h>
#include <inttypes.h>  // For PRIu64

#include <stdexcept>

#include "Vminsoc_bench_core.h"


static uint64_t current_simulation_time = 0;

double sc_time_stamp ()
{
  return double( current_simulation_time );
}


static void ignore_sigpipe ( void )
{
  // Signal SIGPIPE can happen if the JTAG module writes data to a socket and the remote side has already
  // closed the connection. Such errors will be handled properly by the writing routines, but we must ignore
  // the resulting signal, or it will kill the process immediately.

  struct sigaction act;

  act.sa_handler = SIG_IGN;
  act.sa_flags   = 0;

  if ( 0 != sigemptyset( &act.sa_mask ) )
    throw std::runtime_error( "Error setting signal mask." );

  if ( 0 != sigaction( SIGPIPE, &act, NULL ) )
    throw std::runtime_error( "Error setting signal handler." );

  if ( 0 != siginterrupt( SIGPIPE, 0 ) )
    throw std::runtime_error( "Error setting signal interrupt." );
}


int main ( int argc, char ** argv, char ** env )
{
  // The reset level can be positive or negative.
  // At the moment, for MinSoC, the reset level is negative (0 means reset, 1 means no reset).
  const uint8_t RESET_ASSERTED   = 0;
  const uint8_t RESET_DEASSERTED = 1;

  try
  {
    ignore_sigpipe();

    Verilated::commandArgs( argc, argv );  // Remember args for $value$plusargs() and the like.
    Verilated::debug( 0 );  // Comment from Verilator example: "We compiled with it on for testing, turn it back off"

    Vminsoc_bench_core * const top = new Vminsoc_bench_core;

    const uint64_t reset_duration = 10;  // Number of rising clock edges the reset signal will be asserted,
                                         // set it to 0 in order to start the simulation without asserting the reset signal
                                         // (handy to simulate FPGA designs without user reset signal).

    top->reset = reset_duration > 0 ? RESET_ASSERTED : RESET_DEASSERTED;

    while ( !Verilated::gotFinish() )
    {
      // printf( "Iteration, clock: current_simulation_time %" PRIu64 "\n", current_simulation_time );
      // printf( "Reset: %d\n", top->reset );

      if ( current_simulation_time >= reset_duration * 2 )
      {
        top->reset = RESET_DEASSERTED;  // Deassert reset.
      }

      top->clock = !top->clock;

      top->eval();

      ++current_simulation_time;

      // Provide an early warning against the remote possibility of a wrap-around.
      assert( current_simulation_time < UINT64_MAX / 100000 );
    }

    top->final();

    delete top;

    return 0;
  }
  catch ( const std::exception & e )
  {
    fprintf( stderr, "%s%s\n", "ERROR: ", e.what() );
    return 1;
  }
}
