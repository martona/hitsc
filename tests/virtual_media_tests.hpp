#pragma once

namespace hitsc::tests {

// Runs the virtual-media (BlockSource / IsoFileSource / ScsiCdTarget / IUSB framing) unit
// tests. Returns the number of failed checks (0 on success). Called from the hitsc_tests
// main alongside the launcher tests.
int run_virtual_media_tests();

} // namespace hitsc::tests
