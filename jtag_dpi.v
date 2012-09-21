
/* Version 1.0, November 2011.

  See the README file for information about this module.

  Copyright (c) 2011 R. Diez

  This source file may be used and distributed without
  restriction provided that this copyright statement is not
  removed from the file and that any derivative work contains
  the original copyright notice and the associated disclaimer.

  This source file is free software; you can redistribute it
  and/or modify it under the terms of the GNU Lesser General
  Public License version 3 as published by the Free Software Foundation.

  This source is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the GNU Lesser General Public License for more
  details.

  You should have received a copy of the GNU Lesser General
  Public License along with this source; if not, download it
  from http://www.gnu.org/licenses/
*/


// 1/2 of a JTAG TCK clock period will be this many system_clk ticks.
//
// This is an excerpt from the adv_dbg module documentation:
//   Since the JTAG clock is asynchronous to the Wishbone bus clock, transactions
//   are synchronized between clock domains. This clock differential may cause other
//   problems, however, if the JTAG clock is much faster than the Wishbone clock. For
//   efficient operation, it must be possible to complete a Wishbone write (plus 4 JTAG-
//   domain clock cycles for control and synchronization) in less time than it takes to shift the
//   next word in via the TAP. The problem is magnified when using 8-bit words. If the bus
//   is not ready by the time the next word has arrived, then the attempt to write the next word
//   will fail.
//
// Although I am not sure about the exact timings, I think the system_clk clock should be at least
// 8 times faster than the JTAG clock. This is a comment on this matter from Nathan:
//    The debug unit assumes that a memory access can be performed in ~8 JTAG clock cycles.
//    This isn't necessarily the case if the JTAG clock is the same as the
//    wishbone clock (or worse, faster). A factor of 20 is probably not
//    necessary in simulation, but an order-of-magnitude difference is not
//    unreasonable when considering the real system hardware.
//
// Note that 20 means here actually that the JTAG TCK clock will be 40 times slower than system_clk.
`define JTAG_DPI_TCK_HALF_PERIOD_TICK_COUNT 20


module jtag_dpi
  #( LISTENING_TCP_PORT = 4567,
     LISTEN_ON_LOCAL_ADDR_ONLY = 1,  // Whether to listen on localhost / 127.0.0.1 only. Otherwise,
                                     // it listens on all IP addresses, which means any computer
                                     // in the network can connect to the JTAG DPI module.

     PRINT_INFORMATIONAL_MESSAGES = 1,  // The informational messages, if enabled, are printed to stdout. Error messages
                                        // cannot be turned off and get printed to stderr.

     PRINT_RECEIVED_JTAG_DATA = 0
   )
   ( input  system_clk,
     output jtag_tms_o,
     output jtag_tck_o,
     output jtag_trst_o,
     output jtag_tdi_o,
     input  jtag_tdo_i
   );


   reg    received_jtag_tms;
   reg    received_jtag_tck;
   reg    received_jtag_trst;
   reg    received_jtag_tdi;
   reg    received_jtag_new_data_available;

   import "DPI-C" function int jtag_dpi_init ( input integer tcp_port,
                                               input bit listen_on_local_addr_only,
                                               input integer jtag_tck_half_period_tick_count,
                                               input bit print_informational_messages );

   import "DPI-C" function int jtag_dpi_tick ( output bit jtag_tms,
                                               output bit jtag_tck,
                                               output bit jtag_trst,
                                               output bit jtag_tdi,
                                               output bit jtag_new_data_available,
                                               input bit  jtag_tdo );

   // It is not necessary to call jtag_dpi_terminate(). However, calling it
   // will release all resources associated with the JTAG DPI module, and that can help
   // identify resource or memory leaks in other parts of the software.
   import "DPI-C" function void jtag_dpi_terminate ();

   initial
     begin
        jtag_tms_o  = 0;
        jtag_tck_o  = 0;
        jtag_trst_o = 1;  // The JTAG TRST reset signal is active when low.
        jtag_tdi_o  = 0;

        if ( 0 != jtag_dpi_init( LISTENING_TCP_PORT,
                                 LISTEN_ON_LOCAL_ADDR_ONLY,
                                 `JTAG_DPI_TCK_HALF_PERIOD_TICK_COUNT,
                                 PRINT_INFORMATIONAL_MESSAGES ) )
          begin
             $display("Error initializing the JTAG DPI module.");
             $finish;
          end;
     end

   always @ ( posedge system_clk )
     begin
        if ( 0 != jtag_dpi_tick( received_jtag_tms,
                                 received_jtag_tck,
                                 received_jtag_trst,
                                 received_jtag_tdi,
                                 received_jtag_new_data_available,
                                 jtag_tdo_i ) )
          begin
             $display("Error receiving from the JTAG DPI module.");
             $finish;
          end;

        if ( received_jtag_new_data_available )
          begin
             if ( PRINT_RECEIVED_JTAG_DATA )
               begin
                  $display( "JTAG DPI module: Received JTAG data: TCK: %0d, TMS: %0d, TDI: %0d, TRST: %0d.",
                            received_jtag_tck,
                            received_jtag_tms,
                            received_jtag_tdi,
                            received_jtag_trst );
               end

             jtag_tms_o  <= received_jtag_tms;
             jtag_tck_o  <= received_jtag_tck;
             jtag_trst_o <= received_jtag_trst;
             jtag_tdi_o  <= received_jtag_tdi;
          end
     end;

endmodule
