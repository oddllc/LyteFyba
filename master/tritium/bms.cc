#define USE_VOLT_REQ 0		// 1 to send voltage requests every 45 sec to get min and max cell voltage

#include <string.h>			// For strlen() etc
#include <msp430x24x.h>		// For UC1IE etc

#include "tri86.h"
#include "bms.h"
#include "charger.h"
#include "queue.h"
#include "can.h"			// For can_transmit(), but also general definitions
#include "assert2.h"		// assert-like function

// Private function prototypes
bool bmu_sendByte(unsigned char ch);
bool bmu_sendPacket(const unsigned char* ptr);

// Public variables
volatile unsigned int bmu_events = 0;
		 unsigned int bmu_state = 0;
volatile unsigned int bmu_sent_timeout;
volatile unsigned int bmu_vr_count = BMU_VR_SPEED;	// Counts BMU_VR_SPEED to 1 for voltage requests

// BMU buffers
bmu_queue bmu_tx_q(BMU_TX_BUFSZ);
bmu_queue bmu_rx_q(BMU_RX_BUFSZ);
unsigned char bmu_lastrx[BMU_RX_BUFSZ];	// Buffer for the last received BMU response
unsigned char bmu_lastrxidx;			// Index into the above


// BMU private variables
volatile unsigned int  bmu_min_mV = 9999;	// The minimum cell voltage in mV
volatile unsigned int  bmu_max_mV = 0;	// The maximum cell voltage in mV
volatile unsigned int  bmu_min_id = 0;	// Id of the cell with minimum voltage
volatile unsigned int  bmu_max_id = 0;	// Id of the cell with maximum voltage
// Current cell in the current end-of-charge test (send voltage request to this cell next)
unsigned int bmu_curr_cell = 1;			// ID of BMU to send to next
//signed	 int	first_bmu_in_bypass = -1; // Charger end-of-charge test

// Stress table with check bits
static int stressTable[8] = {
			(1<<3) + 0,		// Stress 0   $08
			(3<<3) + 1,		// Stress 1   $19
			(3<<3) + 2,		// Stress 2   $1A
			(1<<3) + 3,		// Stress 3   $0B
			(2<<3) + 4,		// Stress 4   $14
			(0<<3) + 5,		// Stress 5   $05
			(0<<3) + 6,		// Stress 6   $06
			(2<<3) + 7		// Stress 7   $17
};

pid pidCharge(						// State for the PID control algorithm for charge current
//		(int)((3.5/8.0) * 8192),	// Set point will be 3.5 out of 8.0, left shifted by 13 bits
		(int)(1.5*256),				// Kp = 1.5 as s7.8 fixed-point
		(int)(1.0*256),				// Ki = 1.0
		(int)(0.3*256),				// Kd = 0.3
		0);							// Initial "measure"

bmu_queue::bmu_queue(unsigned char sz) : queue(sz) {
	assert2(sz <= BMU_RX_BUFSZ, "bmu::queue buffer size");
};

void bms_init()
{
	bmu_lastrxidx = 0;
#if USE_CKSUM	
	// Turn on checksumming in BMUs.
	// The "k" packet is ignored if BMU checksumming is on (bad checksum), but toggles BMU
	// checksumming on if it was off.
	bmu_sendByte('k'); bmu_sendByte('\r');		// NOTE: can't use bmu_SendPacket as it would insert a
												// checksum giving "kk\r" and having opposite effect
#else
	// Turn off checksumming in BMUs.
	// The "kk" packet toggles BMU checksumming off if it was on (single k command with good checksum),
	// but toggles BMU checksumming twice if it was off, thereby leaving it off.
	bmu_sendPacket((unsigned char*)"kk\r");		// DCU checksumming is off, so it won't change the pkt
#endif
	bmu_sendPacket((unsigned char*)"0K\r");	// Turn on (turn off Killing of) BMU badness sending
#if USE_VOLT_REQ
	bmu_sendVoltReq();						// Send the first voltage request packet;driving or charging
#endif
}

bool bmu_sendByte(unsigned char ch) {
	if (bmu_tx_q.enqueue(ch)) {
    	UC1IE |= UCA1TXIE;                        		// Enable USCI_A1 TX interrupt
		events |= EVENT_ACTIVITY;						// Turn on activity light
		return true;
	}
	return false;
}

// Make a voltage request command at *cmd for cell number cellNo
// Called by bmu_sendVoltReq below
void makeVoltCmd(unsigned char* cmd, int cellNo)
{
	unsigned char* p; p = cmd;
	int n = cellNo;
	int h = n / 100;
	if (h) {						// If any hundreds
		*p++ = h + '0';					// then emit hundreds digit
		n -= h * 100;
	}
	int t = n / 10;
	if (h || t) {					// If hundreds or tens
		*p++ = t + '0';					// then emit tens digit
		n -= t * 10;
	}
	*p++ = n + '0';					// Emit units digit
	*p++ = 's'; *p++ = 'v'; *p++ = '\r'; *p++ = '\0';	// Emit s (select) and v (voltage) cmnds
}														// and terminate with return and null

// Send a request for a voltage reading, to a specific BMU
// Returns true on success
bool bmu_sendVoltReq()
{
	unsigned char cmd[8];
	makeVoltCmd(cmd, bmu_curr_cell);	// cmd := "XXXsv\r"
	return bmu_sendPacket(cmd);
}

// Send min and max cell voltage and ids on CAN bus so telemetry software on PC can display them
void can_sendCellMaxMin(unsigned int bmu_min_mV, unsigned int bmu_max_mV,
								unsigned int bmu_min_id, unsigned int bmu_max_id)
{
	// We have the min and max cell voltage information. Send a CAN packet so the telemetry
	//	software can display them. Use CAN id 0x266, as the IQcell BMS would
	can.identifier = 0x266;
	can.data.data_u16[0] = bmu_min_mV;
	can.data.data_u16[1] = bmu_max_mV;
	can.data.data_u16[2] = bmu_min_id;
	can.data.data_u16[3] = bmu_max_id;
	can_transmit();
}

// Only used for debugging.
// Send total battery voltage as a comment packet on BMU channel
bool bmu_sendVAComment(int nVolt, int nAmp)
{
	// Packet to announce the charger's meas of total voltage and current
	// \ C H G _ n n n V _ n . n A \r
	// 0 1 2 3 4 5 6 7 8 9 a b c d e
	static unsigned char szChgrVolt[16] = "\\CHG nnnV n.nA\r";
	szChgrVolt[5] = nVolt / 1000 + '0';				// Voltage hundreds
	szChgrVolt[6] = (nVolt % 1000) / 100 + '0';		//	tens
	szChgrVolt[7] = (nVolt % 100) / 10 + '0';		//	units
	szChgrVolt[10] = (nAmp / 10) + '0';	// Current units
	szChgrVolt[12] = (nAmp % 10) + '0';	//	tenths
	return bmu_sendPacket(szChgrVolt); // Send as comment packet on BMU channel for debugging
}

void handleBMUstatusByte(unsigned char status, bool bCharging)
{
	int current, output;
	int stress = status & 0x07;			// Isolate stress bits
	int encoded = status & 0x1F;		// Stress bits and check bits
	bool bValid;
	
	
	// Check for validity
	bValid = stressTable[stress] == encoded;

	if (bCharging) {
		// FIXME: not handling comms error bit yet
		if (chgr_state & CHGR_END_CHARGE)
			return;
		if (bValid) {
		    // bmu_sendVAComment(stress*10, 99); // for debugging
			// We need to scale the measurement (stress 0-7) to less than 0..2.0 (shift right by 2),
			// convert to s0.15 fixedpoint (shift left by 15). Overall, shift left by 13 bits.
			// then bias so that the target stress of 3.5 reads as 0 (subtract what 3.5 would come
			// to, which is 3.5 << 13 = 3.5 * 8192 = $7000.
			output = pidCharge.tick((stress << 13) - 0x7000);
			if (status & 0x20 && (chgr_lastCurrent < CHGR_CUT_CURR)) {	// Bit 5 is all in bypass
				if (++chgr_bypCount >= CHGR_EOC_SOAKT) {
					// Terminate charging
					chgr_off();
				}
				else if (chgr_bypCount != 0)			// Care! chgr_bypCount is unsigned
					--chgr_bypCount;					// Saturate at zero
			}
		}
		else {
		    // bmu_sendVAComment(99, 99); // for debugging
			// We have to insert a dummy measurement tick so the derivatives still work
			// Uses the last known good measurement = set_point - prev_error
			output = pidCharge.tick();
		}
		// Scale the output. +1.0 has to correspond to maximum charger current,
		// and -1 to zero current. This is a range of 2^16 (-$8000 .. $7FFF),
		// which we want to map to 0 .. CHGR_CURR_LIMIT.
		// We have a hardware multiplier, so the most efficient way to do this is with a
		// 16x16 bit multiply giving a 32-bit result, and taking the upper half of the result.
		// But we also want to offset the output by 1.0 ($8000).
		// To avoid overflow, we do this with 32-bit arithmetic, i.e.
		//  current = ((out + $8000L) / $10000L) * max
        //			= ((out + $8000L) * max) / $10000L	// Do division last so no fractional intermediate results
        //         =  ((out + $8000L) * max) >> 16		// Do division as shifts, for speed
        // But no actual shifts are required -- just take high word of a long
		// Also add $8000 before the >> 16 for rounding.
		current = ((output + 0x8000L) * CHGR_CURR_LIMIT + 0x8000) >> 16;
		chgr_sendRequest(CHGR_VOLT_LIMIT, current, false);
	} else {
		// Not charging, assume driving.
		// FIXME: TO BE COMPLETED
	}
}

// Read incoming bytes from BMUs
void readBMUbytes(bool bCharging)
{	unsigned char ch;
	while (	bmu_rx_q.dequeue(ch)) {				// Get a byte from the BMU receive queue
		if (ch >= 0x80) {
			handleBMUstatusByte(ch, bCharging);
		} else {
			if (bmu_lastrxidx >= BMU_RX_BUFSZ) {
				fault();
				break;
			}
			bmu_lastrx[bmu_lastrxidx++] = ch;	// !!! Need to check for buffer overflow
			if (ch == '\r')	{					// All BMU responses end with a carriage return
				bmu_processPacket(bCharging);
				break;
			}
		}
	}
}
	
unsigned char bmu_lastSentPacket[BMU_TX_BUFSZ];		// Copy of the last packet sent to the BMUs

// Returns true on success
bool bmu_sendPacket(const unsigned char* ptr)
{
#if USE_CKSUM
	unsigned char ch, i = 0, sum = 0;
	do {
		ch = *ptr++;
		sum ^= ch;									// Calculate XOR checksum
		bmu_lastSentPacket[i++] = ch;				// Copy the data to the transmit buffer
	} while (ch != '\r');
	sum ^= '\r';									// CR is not part of the checksum
	if (sum < ' ') {
		// If the checksum would be a control character that could be confused with a CR, BS,
		//	etc, then send a space, which will change the checksum to a non-control character
		bmu_lastSentPacket[i++-1] = ' ';			// Replace CR with space
		sum ^= ' ';									// Update checksum
	}
	bmu_lastSentPacket[i++-1] = sum;				// Insert the checksum
	bmu_lastSentPacket[i-1] = '\r';					// Add CR
	bmu_lastSentPacket[i] = '\0';					// Null terminate; bmu_resendLastPacket expects this
#else
	strcpy((char*)bmu_lastSentPacket, (char*)ptr);	// Copy the buffer in case we have to resend
#endif
	return bmu_resendLastPacket();					// Call the main transmit function
}

// bmu_resendLastPacket is used for resending after a timeout, but also used for sending the first time.
// Returns true on success
bool bmu_resendLastPacket(void)
{
	int i, len = strlen((char*)bmu_lastSentPacket);
	if ((int)bmu_tx_q.queue_space() < len) {
		fault();
		return false;
	}
	for (i=0; i < len; ++i)							// Send the bytes of the packet
		bmu_sendByte(bmu_lastSentPacket[i]);
	bmu_state |= BMU_SENT;							// Flag that packet is sent but not yet ack'd
	bmu_sent_timeout = BMU_TIMEOUT;					// Initialise timeout counter
	return true;
}


void bmu_processPacket(bool bCharging) {
	bmu_lastrxidx = 0;								// Ready for next BMU response to overwrite this one
													//	(starting next timer interrupt)
	if (chgr_state & CHGR_END_CHARGE)
		return;										// Ignore when charging completed

#if USE_CKSUM
	{	unsigned char sum = 0, j = 0;
		while (bmu_lastrx[j] != '\r')
			sum ^= bmu_lastrx[j++];
		if (sum != 0) {
			// Checksum error; set the error LED and resend the last command
			fault();
	//		bmu_resendLastPacket();					// Resend
			return;									// Don't process this packet
		}
	}
#endif

	// Check for a voltage response
	// Expecting \123:1234 V  ret
	//           0   45    10 11  (note space before the 'V'
	if (bmu_lastrx[0] == '\\' && bmu_lastrx[4] == ':' && bmu_lastrx[10] == 'V') {
		unsigned int bmu_id = 100 * (bmu_lastrx[1] - '0') + (bmu_lastrx[2] - '0') * 10 +
			bmu_lastrx[3] - '0';
		if (bmu_id == bmu_curr_cell) {
			bmu_state &= ~BMU_SENT;				// Call this valid and no longer unacknowledged
			unsigned int rxvolts =
#if 0
				(bmu_lastrx[5] - '0') * 100 +
#else
				// The *50 and << 1 below are to work around a mspgcc bug! See
				// http://sourceforge.net/tracker/index.php?func=detail&aid=2082985&group_id=42303&atid=432701
				(((bmu_lastrx[5] - '0') * 50) << 1) +
#endif
				(bmu_lastrx[6] - '0') * 10 +
				(bmu_lastrx[7] - '0');
			// We expect voltage responses during driving only now
			// We use the voltage measurements to find the min and max cell voltages
			// Get the whole 4-digit number
			rxvolts *= 10; rxvolts += bmu_lastrx[8] - '0';
			if (rxvolts < bmu_min_mV) {
				bmu_min_mV = rxvolts;
				bmu_min_id = bmu_id;
			}
			if (rxvolts > bmu_max_mV) {
				bmu_max_mV = rxvolts;
				bmu_max_id = bmu_id;
			}
			if (bmu_id >= NUMBER_OF_BMUS) {
				// We have the min and max information. Send a CAN packet so the telemetry
				//	software can display them.
				can_sendCellMaxMin(bmu_min_mV, bmu_max_mV, bmu_min_id, bmu_max_id);
			}
			// Move to the next BMU, only if packet valid
			if (++bmu_curr_cell > NUMBER_OF_BMUS)
				bmu_curr_cell = 1;
			else {
				bmu_sendVoltReq();				// Send another voltage request for the next cell
			}
		} // End if (bCharging)
	} // End if valid voltage response		
}


// Called every timer tick from the mainline, for BMU related processing
void bmu_timer() {
	if (bmu_state & BMU_SENT) {
		if (--bmu_sent_timeout == 0) {
			fault();
			bmu_resendLastPacket();			// Resend; will loop until a complete packet is recvd
		}
	}
#if USE_VOLT_REQ
	if (--bmu_vr_count == 0) {
		bmu_vr_count = BMU_VR_SPEED;
		// Reset the min/max data
		bmu_min_mV = 9999;	bmu_max_mV = 0;
		bmu_min_id = 0;		bmu_max_id = 0;
		bmu_curr_cell = 1;
		bmu_sendVoltReq();						// Initiate a chain of voltage requests
	}
#endif
}

