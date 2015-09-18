%include binpac.pac
%include bro.pac

%extern{
	#include "types.bif.h"
	#include "events.bif.h"
%}

analyzer C12_22 withcontext {
	connection: C12_22_Conn;
	flow:       C12_22_Flow;
};

# Our connection consists of two flows, one in each direction.
connection C12_22_Conn(bro_analyzer: BroAnalyzer) {
	upflow   = C12_22_Flow(true);
	downflow = C12_22_Flow(false);
};

%include c12_22-protocol.pac

# Now we define the flow:
flow C12_22_Flow(is_orig: bool) {

	# ## TODO: Determine if you want flowunit or datagram parsing:

	# Using flowunit will cause the anlayzer to buffer incremental input.
	# This is needed for &oneline and &length. If you don't need this, you'll
	# get better performance with datagram.

	# flowunit = C12_22_PDU(is_orig) withcontext(connection, this);
	datagram = C12_22_PDU(is_orig) withcontext(connection, this);

};

%include c12_22-analyzer.pac