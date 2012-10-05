/*
 * Charger interface header file
 *
 * Created 11-Apr-2012
 * by Mike Van Emmerik and Dave Keenan
 *
 */

#include "queue.h"
#include "pid.h"

// Charger constants
#define NUMBER_OF_CELLS		60
#define NUMBER_OF_BMUS		60		// FIXME: testing with 19 BMUs
#define CHGR_VOLT_LIMIT		((int)(NUMBER_OF_CELLS * 34.0))	// Charger voltage limit in tenths of a volt
#define CHGR_CURR_LIMIT		55					// Charger current limit in tenths of an amp
#define CHGR_CURR_DELTA		1					// Amount to increase the current by every second
#define CHGR_EOC_SOAKT		(5 * 60 * BMU_TICK_RATE)// Number of ticks from first detect of all bypass to
												//	turning off the charger
//#define CHGR_SOAK_CURR		5				// Soak mode current in tenths of an ampere
												//	Should be about 1/2 of BMU bypass capacity
#define CHGR_CUT_CURR		20					// Charger cutoff current, usually 0.05C in tenths amp
#define CHGR_TX_BUFSZ		16
#define CHGR_RX_BUFSZ 		16

// Public function prototypes
void chgr_init();								// Once off, "cold" initialising
void chgr_start();								// Called whenever begin charging
void readChargerBytes();
void chgr_sendRequest(int voltage, int current, bool chargerOff);
void chgr_processPacket();
bool chgr_resendLastPacket(void);
void chgr_timer();
void chgr_off();
void handleChargerEvent();
void chgr_setCurrent(unsigned int iCurr);		// Set the current to a particular value (may not send)
void chgr_sendCurrent(unsigned int iCurr);		// Send the current command now


// Public variables
extern volatile unsigned int chgr_events;
extern 			unsigned int chgr_state;
extern unsigned int charger_volt;			// MVE: charger voltage in tenths of a volt
extern unsigned int charger_curr;			// MVE: charger current in tenths of an ampere
extern unsigned char charger_status;		// MVE: charger status (e.g. bit 1 on = overtemp)
extern unsigned int chgr_soakCnt;			// Counter for soak phase
extern unsigned int charger_count;			// Counter to prevent timeout of charger commands
extern unsigned int chgr_lastCurrent;		// Last current commanded from the charger
extern unsigned int chgr_bypCount;			// Balance count in BMU ticks when all in bypass and under
											//	cutoff current
extern pid pidCharge;						// State for the charger pid algorithm



void chgr_timer();						// Called every timer tick, for charger related processing

class chgr_queue : public queue {
	// Allocate space for the real buffer. Note that the base code will use member buf.
	char real_buf[CHGR_RX_BUFSZ];		// Assume that the rx buffer is no smaller than the tx buffer
public:
	chgr_queue(unsigned char sz);
};

// Charger buffers
extern chgr_queue chgr_tx_q;
extern chgr_queue chgr_rx_q;

