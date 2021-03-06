#!/usr/bin/env python
"""TestTidalDropout - test for dropout from TIDAL served tracks.

Parameters:
    arg#1 - Sender DUT
    arg#2 - Receiver/Repeater DUT (None = not present)
    arg#3 - Receiver/SlaveDUT  (None = not present)
    arg#4 - Test duration (secs) or 'forever'
    arg#5 - songcast sender mode (unicast/multicast)
    arg#6 - Number of tracks to test with (use 0 for fixed list of 20 hard-coded tracks)
    arg#7 - Tidal ID
    arg#8 - Tidal username
    arg#9 - Tidal password
local' for internal SoftPlayer on loopback for DUTs

Verifies TIDAL served audio played by the DUT does not suffer from audio dropout.
Additionally checks for dropout on downstream songcast receiver(s)
"""
import _Paths
import CommonTidalDropout as BASE
import sys


class TestTidalDropout( BASE.CommonTidalDropout ):

    def __init__( self ):
        BASE.CommonTidalDropout.__init__( self )
        self.doc = __doc__


if __name__ == '__main__':

    BASE.Run( sys.argv )
