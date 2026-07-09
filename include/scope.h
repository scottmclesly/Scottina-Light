// ============================================================================
//  Scottina Light -- SCOPE BANNER
// ============================================================================
//
//  The Wio Terminal sibling of Scottina (github.com/scottmclesly/Scottina).
//  Same kiosk language -- CRT phosphor palette, semiotic pictograms, navigable
//  tiles that appear as their hardware is detected -- on a 320x240 panel driven
//  by a 5-way switch instead of a touchscreen.
//
//  This firmware is a DIAGNOSTICS-ONLY field instrument.
//
//  Permitted:
//    * Passive observation: I2C bus scan, UART listen/autobaud, CAN passive
//      sniff, onboard sensor reads.
//    * Raw capture to SD, and decode/replay of that capture.
//
//  The ONE sanctioned transmit exception:
//    * CAN heartbeat / ACK replies required to remain a valid node on a live
//      bus. There is deliberately NO code path -- and no UI affordance -- for
//      composing or injecting an arbitrary CAN frame.
//
//  Explicitly out of scope, do not add:
//    * Any offensive or attack tooling; arbitrary frame injection.
//    * Network mapping (ARP sweep, port scan, packet capture, host
//      enumeration). The RTL8720DN AT-firmware layer cannot do these; the
//      Wi-Fi surface is limited to scan / connect / report-own-IP.
//
//  Full specification: WioTerminal-Island-v1-TODO.md
//
//  Standalone by design: depends on Scottina, Scottina Light and CanTick for
//  nothing. It boots, scans, and logs on its own.
// ============================================================================

#ifndef SL_SCOPE_H
#define SL_SCOPE_H

#define SL_PRODUCT "Scottina Light"
// Splash wordmark: "SCOTTINA" set large with "LIGHT" spaced beneath it, the way
// the mother's ScottinaSplash.png stacks the name.
#define SL_PRODUCT_MARK "SCOTTINA"
#define SL_TAGLINE "Pocket digital beast of burden"
#define SL_BYLINE "by Scott McLeslie"
#define SL_VERSION "v1-foundation"

#endif // SL_SCOPE_H
