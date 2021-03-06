#ifndef gctools_interrupt_H
#define gctools_interrupt_H

namespace gctools {

  void clasp_interrupt_process(mp::Process_sp process, core::T_sp function);
  
  void handle_or_queue(core::ThreadLocalState* thread, core::T_sp signal_code /*, int code */);
  void initialize_signals(int clasp_signal);
  void initialize_signal_constants();
  
};

#endif
