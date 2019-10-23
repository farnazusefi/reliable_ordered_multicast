#include <time.h>
#include "net_include.h"
#include "recv_dbg.h"
#include "log.h"

#define TIMEOUT 20000
#define WINDOW_SIZE 80
#define NUM_OF_FINALIZE_MSGS_BEFORE_EXIT 1
#define FLOW_CONTROL_VALVE 10000

typedef struct messageT {
	u_int32_t type;
	u_int32_t pid;
	u_int32_t lastDeliveredCounter;
	char *data;
} message;

typedef struct windowSlotT {
	u_int32_t index;
	u_int32_t randomNumber;
	u_int32_t lamportCounter;
	u_int32_t valid;
	struct timeval fbTimer;
} windowSlot;

typedef struct dataMessageT {
	u_int32_t type;
	u_int32_t pid;
	u_int32_t lastDeliveredCounter;
	u_int32_t index;
	u_int32_t lamportCounter;
	u_int32_t randomNumber;

	char *garbage;
} dataMessage;

typedef struct pollMessageT {
	u_int32_t type;
	u_int32_t pid;
	u_int32_t lastDeliveredCounter;
	u_int32_t pollPID;
} pollMessage;

enum STATE {
	STATE_WAITING, STATE_SENDING, // sending and receiving
	STATE_RECEIVING,
	STATE_FINALIZING
};

enum FEEDBACK {
	FEEDBACK_NACK, FEEDBACK_ACK
};

enum TYPE {
	TYPE_START = 0x00000000,
	TYPE_DATA = 0x00000001,
	TYPE_FEEDBACK = 0x00000002,
	TYPE_POLL = 0x00000003,
	TYPE_FINALIZE = 0x00000004
};

typedef struct sessionT {
	enum STATE state;
	int numberOfMachines;
	int delay;
	windowSlot **dataMatrix;
	u_int32_t *lastInOrderReceivedIndexes;
	u_int32_t *highestReceivedIndexes;
	u_int32_t *windowStartPointers;
	u_int32_t *lastDeliveredCounters;
	u_int32_t *lastDeliveredIndexes;
	u_int32_t *fullyDeliveredProcess;
	u_int32_t *lastExpectedIndexes;

	struct timeval *timoutTimestamps;

	u_int32_t isFinalDelivery;
	u_int32_t localClock;
	u_int32_t machineIndex;
	u_int32_t numberOfPackets;
	u_int32_t lossRate;
	u_int32_t windowSize;
	u_int32_t lastSentIndex;
	u_int32_t lastDeliveredPointer;
	u_int32_t totalPacketsSent;
	u_int32_t totalRetrasmissions;
	u_int32_t totalFeedbacks;
	u_int32_t totalPolls;
	int exitCounter;
	struct timeval exitTimestamp, start, end;

	int sendingSocket;
	int receivingSocket;

	FILE *f;
	struct sockaddr_in sendAddr;

} session;

typedef struct fileToReceiveT {
	int fileDescriptor;
	FILE *fw; /* Pointer to dest file, which we write  */
	unsigned long totalLinesWritten;
} fileToReceive;

void prepareFile();

u_int32_t getMinOfArray(u_int32_t *lastDeliveredCounters, int includeSelf);

int parse(void *buf, int bytes);

void handleStartMessage(message *m, int bytes);

int handleDataMessage(void *m, int bytes);

int handleFeedbackMessage(char *m, int bytes, u_int32_t pid);

int handleFinalizeMessage(void *m, int bytes);

int handlePollMessage(void *m, int bytes);

void handleTimeOut(u_int32_t pid);

void startSending();

void synchronizeWindow();

void initializeBuffers();

void doTerminate();

void initializeAndSendRandomNumber(int moveStartPointer,
		u_int32_t destinationPtr);

void sendMessage(enum TYPE type, char *dp, int payloadSize);

int putInBuffer(dataMessage *m);

void updateLastReceivedIndex(u_int32_t pid);

void attemptDelivery();

int updateLastDeliveredCounter(u_int32_t pid, u_int32_t lastDeliveredCounter);

void deliverToFile(u_int32_t pid, u_int32_t index, u_int32_t randomData);

void sendNack(u_int32_t pid, u_int32_t *indexes, u_int32_t length);

u_int32_t getPointerOfIndex(u_int32_t index);

void resendMessage(u_int32_t index);

void reinitialize();

void busyWait();

session currentSession;

void driveMachine() {
	fd_set mask;
	fd_set dummy_mask, temp_mask;
	int bytes;
	int num;
	char mess_buf[MAX_MESS_LEN];
	struct timeval timeout;
	int terminate = 0;

	log_info("Waiting for start message");
	bytes = recv(currentSession.receivingSocket, mess_buf, sizeof(mess_buf), 0);
	mess_buf[bytes] = 0;
	log_debug("received : %s\n", mess_buf);
	parse((void*) mess_buf, bytes);

	FD_ZERO(&mask);
	FD_ZERO(&dummy_mask);
	FD_SET(currentSession.receivingSocket, &mask);
	for (;;) {
		temp_mask = mask;
		timeout.tv_sec = 0;
		timeout.tv_usec = 10000;
		log_debug("selecting ...");
		num = select(FD_SETSIZE, &temp_mask, &dummy_mask, &dummy_mask,
				&timeout);
		if (num > 0) {
			if (FD_ISSET(currentSession.receivingSocket, &temp_mask)) {
				bytes = recv_dbg(currentSession.receivingSocket, mess_buf,
						sizeof(mess_buf), 0); //TODO change to recv_dbg
				mess_buf[bytes] = 0;
				log_debug("received : %s\n", mess_buf);
				terminate = parse((void*) mess_buf, bytes);
				if (terminate)
					return;
			}
		} else // timeout for select
		{
			int i;
//			if (currentSession.state == STATE_FINALIZING
//					&& currentSession.exitCounter
//					&& getMinOfArray(currentSession.lastDeliveredCounters, 0)
//							== (currentSession.lastDeliveredCounters[currentSession.machineIndex
//									- 1])) {
//				struct timeval current;
//				gettimeofday(&current, NULL);
//				if (current.tv_sec
//						- currentSession.exitTimestamp.tv_sec> WAIT_BEFORE_EXIT) {
//					doTerminate();
//					return;
//				}
//
//			}

			log_debug("timeout in select. Polling all processes");
			if (currentSession.state == STATE_WAITING)
				continue;
			for (i = 1; i <= currentSession.numberOfMachines; i++) {
				if (i != currentSession.machineIndex)
					handleTimeOut(i);

			}
		}
	}
}

int main(int argc, char **argv) {
	struct sockaddr_in name;

	int mcast_addr;
	struct ip_mreq mreq;
	unsigned char ttl_val;

	int debug_mode = 2;
	mcast_addr = 225 << 24 | 1 << 16 | 3 << 8 | 50; /* (225.1.3.50) */

	if (argc != 5 && argc != 7) {
		printf(
				"Usage: ./mcast <num of packets> <machine index> <num of machines> <loss rate> [delay] [debug mode(1-5)]  \n");
		exit(1);
	}
	currentSession.delay = FLOW_CONTROL_VALVE;
	// optional args
	if (argc == 7) {
		debug_mode = atoi(argv[6]);
		currentSession.delay = atoi(argv[5]);

		log_info("debug mode = %d, delay = %d", debug_mode,
				currentSession.delay);
	}
	log_set_level(debug_mode);
	currentSession.numberOfPackets = atoi(argv[1]);
	currentSession.machineIndex = atoi(argv[2]);
	currentSession.numberOfMachines = atoi(argv[3]);
	currentSession.lossRate = atoi(argv[4]);
	log_info(
			"number of packets = %d, machine index = %d number of machines = %d  loss rate = %d",
			currentSession.numberOfPackets, currentSession.machineIndex,
			currentSession.numberOfMachines, currentSession.lossRate);

	recv_dbg_init(currentSession.lossRate, currentSession.machineIndex);

	currentSession.receivingSocket = socket(AF_INET, SOCK_DGRAM, 0); /* socket for receiving */
	if (currentSession.receivingSocket < 0) {
		perror("Mcast: socket");
		exit(1);
	}

	name.sin_family = AF_INET;
	name.sin_addr.s_addr = INADDR_ANY;
	name.sin_port = htons(PORT);

	if (bind(currentSession.receivingSocket, (struct sockaddr*) &name,
			sizeof(name)) < 0) {
		perror("Mcast: bind");
		exit(1);
	}

	mreq.imr_multiaddr.s_addr = htonl(mcast_addr);

	/* the interface could be changed to a specific interface if needed */
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	if (setsockopt(currentSession.receivingSocket, IPPROTO_IP,
	IP_ADD_MEMBERSHIP, (void*) &mreq, sizeof(mreq)) < 0) {
		perror("Mcast: problem in setsockopt to join multicast address");
	}

	currentSession.sendingSocket = socket(AF_INET, SOCK_DGRAM, 0); /* Socket for sending */

	if (currentSession.sendingSocket < 0) {
		perror("Mcast: socket");
		exit(1);
	}

	ttl_val = 1;
	if (setsockopt(currentSession.sendingSocket, IPPROTO_IP, IP_MULTICAST_TTL,
			(void*) &ttl_val, sizeof(ttl_val)) < 0) {
		log_warn(
				"Mcast: problem in setsockopt of multicast ttl %d - ignore in WinNT or Win95\n",
				ttl_val);
	}

	currentSession.sendAddr.sin_family = AF_INET;
	currentSession.sendAddr.sin_addr.s_addr = htonl(mcast_addr); /* mcast address */
	currentSession.sendAddr.sin_port = htons(PORT);

	currentSession.state = STATE_WAITING;

	initializeBuffers();

	log_trace("printing trace logs");
	log_debug("printing debug logs");
	log_info("printing info logs");
	log_warn("printing warning logs");
	log_error("printing error logs");
	log_fatal("noting fatal will hopefully occur!");

	while (1)
		driveMachine();

	return 0;
}

void initializeBuffers() {

	u_int32_t i;
	log_info("initializing buffers");
	currentSession.lastDeliveredCounters = (u_int32_t*) calloc(
			currentSession.numberOfMachines, sizeof(u_int32_t));
	currentSession.lastInOrderReceivedIndexes = (u_int32_t*) calloc(
			currentSession.numberOfMachines, sizeof(u_int32_t));
	currentSession.windowStartPointers = (u_int32_t*) calloc(
			currentSession.numberOfMachines, sizeof(u_int32_t));
	currentSession.highestReceivedIndexes = (u_int32_t*) calloc(
			currentSession.numberOfMachines, sizeof(u_int32_t));
	currentSession.lastDeliveredIndexes = (u_int32_t*) calloc(
			currentSession.numberOfMachines, sizeof(u_int32_t));
	currentSession.timoutTimestamps = (struct timeval*) malloc(
			currentSession.numberOfMachines * sizeof(struct timeval));
	currentSession.windowSize = WINDOW_SIZE;
	currentSession.fullyDeliveredProcess = (u_int32_t*) calloc(
			currentSession.numberOfMachines, sizeof(u_int32_t));
	currentSession.lastExpectedIndexes = (u_int32_t*) calloc(
			currentSession.numberOfMachines, sizeof(u_int32_t));

	currentSession.dataMatrix = (windowSlot**) malloc(
			currentSession.numberOfMachines * sizeof(windowSlot*));

	for (i = 0; i < currentSession.numberOfMachines; i++) {
		currentSession.dataMatrix[i] = (windowSlot*) calloc(
				currentSession.windowSize, sizeof(windowSlot));
	}
	currentSession.totalPacketsSent = 0;
	currentSession.totalFeedbacks = 0;
	currentSession.totalPolls = 0;
	currentSession.totalRetrasmissions = 0;
	currentSession.localClock = 0;
	currentSession.lastSentIndex = 0;
	currentSession.isFinalDelivery = 0;
	currentSession.lastDeliveredPointer = 0;
	currentSession.exitCounter = 0;
	srand(time(0));
}

void handleTimeOut(u_int32_t pid) {
	char pollMsg[4];
	log_debug("Sending POLL for pid=%d", pid);
	memcpy(pollMsg, &pid, 4);
	currentSession.totalPolls++;
	sendMessage(TYPE_POLL, pollMsg, 4);
}

void checkTimeoutForOthers() {
	int i;
	struct timeval t;
	log_debug("Checking to see if we should poll any process");
	gettimeofday(&t, NULL);
	for (i = 0; i < currentSession.numberOfMachines; i++) {
		if ((t.tv_sec - currentSession.timoutTimestamps[i].tv_sec) * 1000000
				+ (t.tv_usec - currentSession.timoutTimestamps[i].tv_usec)
				> TIMEOUT && i != currentSession.machineIndex - 1) {
			currentSession.timoutTimestamps[i] = t;
			handleTimeOut(i + 1);
		}
	}
}

int parse(void *buf, int bytes) {
	message *m = (message*) buf;
	int terminated = 0;
	if (m->pid == currentSession.machineIndex)
		return 0;
	log_debug("parsing ...");
	switch (m->type) {
	case TYPE_START:
		handleStartMessage(m, bytes);
		break;
	case TYPE_DATA:
		terminated = handleDataMessage(buf, bytes);
		break;
	case TYPE_FEEDBACK:
		terminated = handleFeedbackMessage(buf, bytes, m->pid);
		break;
	case TYPE_FINALIZE:
		terminated = handleFinalizeMessage(buf, bytes);
		break;
	case TYPE_POLL:
		terminated = handlePollMessage(buf, bytes);
		break;
	default:
		log_warn("invalid type %d\n", m->type);
		break;
	}
	if (m->type != TYPE_START) {
		gettimeofday(&currentSession.timoutTimestamps[m->pid - 1], NULL);
		checkTimeoutForOthers();
	}
	return terminated;
}

int handlePollMessage(void *m, int bytes) {
	pollMessage *message = (pollMessage*) m;
	u_int32_t polledPid = message->pollPID;
	int terminated = 0;
	log_debug("handling poll message from %d for %d", message->pid, polledPid);

	if (currentSession.state == STATE_WAITING) {
		resendMessage(currentSession.lastSentIndex);
		return 0;
	}

	terminated = updateLastDeliveredCounter(message->pid,
			message->lastDeliveredCounter);
	if (terminated)
		return 1;
	if (polledPid != currentSession.machineIndex)
		return 0;

	if (currentSession.state == STATE_RECEIVING) {
		char data[1412];
		u_int32_t zero = 0;
		memcpy(data, &zero, 4);
		currentSession.totalFeedbacks++;
		sendMessage(TYPE_FINALIZE, data, 1412);
	} else
		resendMessage(currentSession.lastSentIndex);
	return 0;
}

int handleFinalizeMessage(void *m, int bytes) {

	dataMessage *dm = (dataMessage*) m;
	int terminated = 0;
	log_debug("received finalize message from %d, with index %d", dm->pid,
			dm->index);
	currentSession.fullyDeliveredProcess[dm->pid - 1] = 1;
	if (dm->index == 0) {
//		currentSession.lastExpectedIndexes[dm->pid - 1] = 1;
		terminated = updateLastDeliveredCounter(dm->pid,
				dm->lastDeliveredCounter);
		return terminated;
	}
	currentSession.lastExpectedIndexes[dm->pid - 1] = dm->index;
	return handleDataMessage(m, bytes);
}

void resendMessage(u_int32_t index) {
	u_int32_t type = TYPE_DATA;
	windowSlot *myDataArray =
			currentSession.dataMatrix[currentSession.machineIndex - 1];
	windowSlot ws = myDataArray[getPointerOfIndex(index)];
	char data[1412];
	log_debug("re-sending data index %d", index);

	memcpy(data, &ws.index, 4);
	memcpy(data + 4, &ws.lamportCounter, 4);
	memcpy(data + 8, &ws.randomNumber, 4);

	memcpy(data + 12, garbage_data, 1400);

	if (index == currentSession.numberOfPackets)
		type = TYPE_FINALIZE;
	currentSession.totalRetrasmissions++;
	sendMessage(type, data, 1412);
}

int handleFeedbackMessage(char *m, int bytes, u_int32_t pid) {
	u_int32_t feedBackType;
	memcpy(&feedBackType, m + 12, 4);
	u_int32_t lastDeliveredCounter;
	u_int32_t numOfNacks;
	u_int32_t i;
	u_int32_t machineIdx;
	int terminated = 0;
	switch (feedBackType) {
	case FEEDBACK_ACK:
		memcpy(&lastDeliveredCounter, m + 16, 4);
		log_debug("handling Ack for counter %d from process %d",
				lastDeliveredCounter, pid);
		terminated = updateLastDeliveredCounter(pid, lastDeliveredCounter);
		currentSession.fullyDeliveredProcess[pid - 1] = 1;
		break;
	case FEEDBACK_NACK:
		memcpy(&machineIdx, m + 16, 4);
		if (machineIdx == currentSession.machineIndex) {
			memcpy(&numOfNacks, m + 20, 4);
			log_debug(
					"handling Nack for of length %d from process %d, totaling %d bytes",
					numOfNacks, pid, bytes);
			for (i = 0; i < numOfNacks; i++) {
				u_int32_t index;
				memcpy(&index, m + 12 + (4 * (i + 3)), 4);
				resendMessage(index);
			}
		}
		break;
	default:
		break;
	}
	return terminated;
}

int updateLastDeliveredCounter(u_int32_t pid, u_int32_t lastDeliveredCounter) {
	log_debug(
			"attempting to update last delivered counter for process %d, last counter received = %d, my last value from her = %d",
			pid, lastDeliveredCounter,
			currentSession.lastDeliveredCounters[pid - 1]);
	if (currentSession.lastDeliveredCounters[pid - 1] < lastDeliveredCounter) {
		currentSession.lastDeliveredCounters[pid - 1] = lastDeliveredCounter;
		synchronizeWindow();
	}
	log_debug("update last delivered ctr, min of array %d , my last ctr %d",
			getMinOfArray(currentSession.lastDeliveredCounters, 0),
			(currentSession.lastDeliveredCounters[currentSession.machineIndex
					- 1]));
	if (currentSession.state == STATE_FINALIZING
			&& getMinOfArray(currentSession.lastDeliveredCounters, 0)
					== (currentSession.lastDeliveredCounters[currentSession.machineIndex
							- 1])) {
		currentSession.exitCounter++;
		if (currentSession.exitCounter == 1) {
			gettimeofday(&currentSession.exitTimestamp, NULL);
			log_info(
					"Termination conditions are okay for me. Getting ready to terminate ...");
		}
		if (currentSession.exitCounter >= NUM_OF_FINALIZE_MSGS_BEFORE_EXIT) {
			doTerminate();
			return 1;
		}
	}
	return 0;
}

int timediff_us(struct timeval tv2, struct timeval tv1) {
	return ((tv2.tv_sec - tv1.tv_sec) * 1000000) + (tv2.tv_usec - tv1.tv_usec);
}

int handleDataMessage(void *m, int bytes) {
	dataMessage *dm = (dataMessage*) m;
	struct timeval now;
	log_debug("handling data with counter= %d, index = %d from process %d",
			dm->lamportCounter, dm->index, dm->pid);
	switch (currentSession.state) {
	case STATE_RECEIVING:
	case STATE_SENDING:
		if (dm->lamportCounter > currentSession.localClock)
			currentSession.localClock = dm->lamportCounter;
		putInBuffer(dm);
		if (dm->index
				> currentSession.lastInOrderReceivedIndexes[dm->pid - 1]) {
			u_int32_t currentPointer = getPointerOfIndex(dm->index);
			u_int32_t nackIndices[currentSession.windowSize];
			int counter = 0;
			int indexDistance = 0;
			gettimeofday(&now, NULL);
			while (currentPointer
					!= getPointerOfIndex(
							currentSession.lastInOrderReceivedIndexes[dm->pid
									- 1])) {
				u_int32_t diff =
						timediff_us(now,
								currentSession.dataMatrix[dm->pid - 1][currentPointer].fbTimer);
				log_trace(
						"time difference for sending nack is %d for pid %d current ptr %d",
						diff, dm->pid, currentPointer);
				if (!currentSession.dataMatrix[dm->pid - 1][currentPointer].valid
						&& diff > 5000) {
					nackIndices[counter] = dm->index - indexDistance;
					gettimeofday(
							&currentSession.dataMatrix[dm->pid - 1][currentPointer].fbTimer,
							NULL);
					counter++;
				}
				currentPointer = (currentPointer - 1);
				if (currentPointer == -1)
					currentPointer = currentSession.windowSize - 1;
				indexDistance++;

			}
			if (!currentSession.lastInOrderReceivedIndexes[dm->pid - 1])// when packet with index 1 is lost!!!
			{
				if (!currentSession.dataMatrix[dm->pid - 1][0].valid) {
					nackIndices[counter] = 1;
					gettimeofday(&currentSession.dataMatrix[dm->pid - 1][currentPointer].fbTimer,NULL);
					counter++;
				}
				indexDistance++;
			}
			if (counter > 0)
				sendNack(dm->pid, nackIndices, counter);
		}

		return updateLastDeliveredCounter(dm->pid, dm->lastDeliveredCounter);

		break;
	case STATE_FINALIZING:
		return updateLastDeliveredCounter(dm->pid, dm->lastDeliveredCounter);
	default:
		log_warn("discarding unexpected data");
		break;
	}
	return 0;
}
u_int32_t getMinOfArray(u_int32_t *lastDeliveredCounters, int includeSelf) {
	u_int32_t min = -1;
	int i;
	for (i = 0; i < currentSession.numberOfMachines; i++) {
		if (!includeSelf && i == currentSession.machineIndex - 1)
			continue;
		if (currentSession.lastDeliveredCounters[i] < min)
			min = currentSession.lastDeliveredCounters[i];
	}
	return min;
}

void synchronizeWindow() {
	u_int32_t minimumOfWindow = getMinOfArray(
			currentSession.lastDeliveredCounters, 1);
	log_debug("Synchronizing my window, minimum delivered counter is %d",
			minimumOfWindow);

	while (currentSession.lastSentIndex < currentSession.numberOfPackets) {
		u_int32_t windowStartPointer =
				currentSession.windowStartPointers[currentSession.machineIndex
						- 1];
		log_trace("Synchronizing my window, window start ptr ctr is %d",
				currentSession.dataMatrix[currentSession.machineIndex - 1][windowStartPointer].lamportCounter);
		if (currentSession.dataMatrix[currentSession.machineIndex - 1][windowStartPointer].lamportCounter
				<= minimumOfWindow && currentSession.state == STATE_SENDING) {
			initializeAndSendRandomNumber(1, 0);
			attemptDelivery();
		} else
			break;
	}

}

void sendNack(u_int32_t pid, u_int32_t *indexes, u_int32_t length) {
	char data[12 + sizeof(u_int32_t) * length];
	u_int32_t feedbackType = FEEDBACK_NACK;
	memcpy(data, &feedbackType, 4);
	memcpy(data + 4, &pid, 4);
	memcpy(data + 8, &length, 4);
	int i;
	for (i = 1; i <= length; i++) {
		memcpy(data + ((4 * i) + 8), &indexes[length - i], 4);
		log_debug("sending NACK for %d messages, index=%d", length,
				indexes[length - i]);
	}
	currentSession.totalFeedbacks++;
	sendMessage(TYPE_FEEDBACK, data, 12 + sizeof(u_int32_t) * length);
}

u_int32_t getPointerOfIndex(u_int32_t index) {
	if (!index)
		return 0;
	return (index - 1) % currentSession.windowSize;
}

void sendAck() {
	char data[8];
	u_int32_t feedbackType = FEEDBACK_ACK;
	memcpy(data, &feedbackType, 4);
	memcpy(data + 4,
			&currentSession.lastDeliveredCounters[currentSession.machineIndex
					- 1], 4);
	log_debug("Acknowledging data for clock %d",
			currentSession.lastDeliveredCounters[currentSession.machineIndex - 1]);
	currentSession.totalFeedbacks++;
	sendMessage(TYPE_FEEDBACK, data, 8);
}

int putInBuffer(dataMessage *m) {
	windowSlot *currentWindow = currentSession.dataMatrix[m->pid - 1];
	windowSlot ws;
	u_int32_t startIndex = currentSession.lastDeliveredIndexes[m->pid - 1] + 1;
	log_debug(
			"putInBuffer Condition Check, last inorder received idx = %d, startIndex = %d",
			currentSession.lastInOrderReceivedIndexes[m->pid - 1], startIndex);
// Check if the received packet's index is in the valid range for me to store
	if (m->index > currentSession.lastInOrderReceivedIndexes[m->pid - 1]
			&& m->index < (startIndex + currentSession.windowSize)) {
		if (currentWindow[getPointerOfIndex(m->index)].valid) {
			log_debug(
					"not putting in buffer (already in window), counter %d, index %d from process %d",
					m->lamportCounter, m->index, m->pid);
			return 0;
		}
		log_debug(
				"putting in buffer, counter %d, index %d from process %d to position %d",
				m->lamportCounter, m->index, m->pid,
				getPointerOfIndex(m->index));
		ws.index = m->index;
		ws.lamportCounter = m->lamportCounter;
		ws.randomNumber = m->randomNumber;
		ws.valid = 1;
		gettimeofday(&ws.fbTimer, NULL);
		currentWindow[getPointerOfIndex(m->index)] = ws;
		updateLastReceivedIndex(m->pid);
		attemptDelivery();
		if (currentSession.numberOfPackets == 0) {
			sendAck();
		}
		return 1;
	}
	log_debug("not putting in buffer, counter %d, index %d from process %d",
			m->lamportCounter, m->index, m->pid);
	return 0;
}

void doTerminate() {
	gettimeofday(&currentSession.end, NULL);
	u_int32_t duration = ((currentSession.end.tv_sec
			- currentSession.start.tv_sec) * 1000000)
			+ (currentSession.end.tv_usec - currentSession.start.tv_usec);
	log_info("termination conditions hold.");
	log_info("Total elapsed time: %d seconds and %d miliseconds",
			duration / 1000000, (duration % 1000000) / 1000);
	log_info(
			"Total packets sent: %d - retransmissions = %d - polls = %d - feedbacks = %d",
			currentSession.totalPacketsSent, currentSession.totalRetrasmissions,
			currentSession.totalPolls, currentSession.totalFeedbacks);
	fclose(currentSession.f);
	currentSession.state = STATE_WAITING;
	log_info("Exiting, BYE!");
//	exit(0);
}

int dataRemaining() {
	int i, terminationCtr = 0;
	u_int32_t pointer;
	for (i = 0; i < currentSession.numberOfMachines; i++) {
		log_trace("data remaining? process %d, fully delivered = %d", i + 1,
				currentSession.fullyDeliveredProcess[i]);
		if (currentSession.fullyDeliveredProcess[i]) {
			log_trace(
					"data remaining? process %d, it is fully delivered, last delivered index = %d, last expected index = %d",
					i + 1, currentSession.lastDeliveredIndexes[i],
					currentSession.lastExpectedIndexes[i]);
			if (currentSession.lastDeliveredIndexes[i]
					== currentSession.lastExpectedIndexes[i]) {
				terminationCtr++;
				continue;
			}
		}
		if (currentSession.machineIndex - 1 == i) {
			log_trace(
					"data remaining? process %d (myself), last sent idx = %d, last delivered idx = %d",
					i + 1, currentSession.lastSentIndex,
					currentSession.lastDeliveredIndexes[i]);
			if (currentSession.lastSentIndex
					== currentSession.lastDeliveredIndexes[i])
				return 0;
			continue;
		}
		pointer = getPointerOfIndex(currentSession.lastDeliveredIndexes[i] + 1);
		log_trace(
				"data remaining? process %d, valid? = %d, last delivered idx+1 ptr = %d",
				i + 1, currentSession.dataMatrix[i][pointer].valid, pointer);
		if (!currentSession.dataMatrix[i][pointer].valid)
			return 0;
	}
	if (terminationCtr == currentSession.numberOfMachines) {
		if (currentSession.state != STATE_FINALIZING)
			currentSession.lastDeliveredCounters[currentSession.machineIndex - 1]++;
		currentSession.state = STATE_FINALIZING;
		return 0;
	}
	return 1;
}

void getLowestToDeliver(u_int32_t *pid, u_int32_t *pointer) {
	int i;
	u_int32_t minimumClock = -1;
	for (i = 0; i < currentSession.numberOfMachines; i++) {
		// 			go forward in window till you reach an undelivered slot
		u_int32_t nextReadyForDeliveryPtr;
		if (currentSession.fullyDeliveredProcess[i]
				&& currentSession.lastDeliveredIndexes[i]
						== currentSession.lastExpectedIndexes[i])
			continue;
		nextReadyForDeliveryPtr = getPointerOfIndex(
				currentSession.lastDeliveredIndexes[i] + 1);
		log_debug("our window in pointer %d contains index %d valid %d",
				nextReadyForDeliveryPtr,
				currentSession.dataMatrix[i][nextReadyForDeliveryPtr].index,
				currentSession.dataMatrix[i][nextReadyForDeliveryPtr].valid);
		if (currentSession.dataMatrix[i][nextReadyForDeliveryPtr].lamportCounter
				< minimumClock) {
			minimumClock =
					currentSession.dataMatrix[i][nextReadyForDeliveryPtr].lamportCounter;
			*pointer = nextReadyForDeliveryPtr;
			*pid = i + 1;
			log_debug("changing minimum: pointer %d pid %d",
					nextReadyForDeliveryPtr, i + 1);
		}

	}
	log_debug("lowest to deliver is: pointer %d pid %d", *pointer, *pid);

}

void attemptDelivery() {
	log_debug("checking delivery conditions");
	u_int32_t pointer, pid;
	windowSlot ws;
	while (dataRemaining()) {
		getLowestToDeliver(&pid, &pointer);
		ws = currentSession.dataMatrix[pid - 1][pointer];
		log_debug(
				"delivering to file, counter %d, index %d from process %d, data: %d",
				ws.lamportCounter, ws.index, pid, ws.randomNumber);
		currentSession.lastDeliveredCounters[currentSession.machineIndex - 1] =
				ws.lamportCounter - 1;
		deliverToFile(pid, ws.index, ws.randomNumber);

	}
}

void updateLastReceivedIndex(u_int32_t pid) {

	windowSlot *currentWindowSlots = currentSession.dataMatrix[pid - 1];
	u_int32_t lastValidIndex =
			currentSession.lastInOrderReceivedIndexes[pid - 1];
	u_int32_t windowStartPointer = currentSession.windowStartPointers[pid - 1];
	u_int32_t lastValidIndexPointer = getPointerOfIndex(lastValidIndex);
	log_debug(
			"attempting to Update last received index for %d - lastvalididxptr = %d, window startptr = %d",
			pid, lastValidIndexPointer, windowStartPointer);
	if (lastValidIndex == 0) {
		currentSession.lastInOrderReceivedIndexes[pid - 1] = 1;
		log_debug("Updating last received index to 1");
		return;
	}
	u_int32_t searchingPointer = windowStartPointer;
	u_int32_t counter = 0;
	while (counter != currentSession.windowSize) {
		counter++;
		if (currentWindowSlots[searchingPointer].valid) {
			currentSession.lastInOrderReceivedIndexes[pid - 1] =
					currentWindowSlots[searchingPointer].index;
			searchingPointer = (searchingPointer + 1)
					% currentSession.windowSize;
		} else {
			break;
		}
	}
	log_debug("Updating last received index to %d",
			currentSession.lastInOrderReceivedIndexes[pid - 1]);

}

void handleStartMessage(message *m, int bytes) {
	struct timeval t;
	int i;
	log_info("handling start message. Starting ...");
	switch (currentSession.state) {
	case STATE_WAITING:
//		reinitialize();
		prepareFile();
		gettimeofday(&currentSession.start, NULL);
		gettimeofday(&t, NULL);
		for (i = 0; i < currentSession.numberOfMachines; i++) {
			currentSession.timoutTimestamps[i] = t;
		}
		if (currentSession.numberOfPackets > 0) {
			currentSession.state = STATE_SENDING;
			startSending();
		} else {
			currentSession.state = STATE_RECEIVING;
			currentSession.fullyDeliveredProcess[currentSession.machineIndex - 1] =
					1;
		}
		break;
	default:
		log_warn("the process has already started");
		break;
	}
}

void startSending() {
	int i;
	log_debug("Sending our window");
	for (i = 0; i < currentSession.windowSize; i++) {
		initializeAndSendRandomNumber(0, i);
	}
}

void initializeAndSendRandomNumber(int moveStartpointer,
		u_int32_t destinationPtr) {
	windowSlot ws;
	u_int32_t type = TYPE_DATA;
	u_int32_t randomNumber = rand() % 1000000;
	char data[1412];
	++currentSession.lastSentIndex;
	++currentSession.localClock;
	memcpy(data, &currentSession.lastSentIndex, 4);
	memcpy(data + 4, &currentSession.localClock, 4);
	memcpy(data + 8, &randomNumber, 4);
	ws.index = currentSession.lastSentIndex;
	ws.lamportCounter = currentSession.localClock;
	ws.randomNumber = randomNumber;
	ws.valid = 1;
	gettimeofday(&ws.fbTimer, NULL);
	if (!moveStartpointer) {
		currentSession.dataMatrix[currentSession.machineIndex - 1][destinationPtr] =
				ws;
	} else {
		u_int32_t ourMachinePointer = currentSession.machineIndex - 1;
		currentSession.dataMatrix[ourMachinePointer][currentSession.windowStartPointers[ourMachinePointer]] =
				ws;
		currentSession.windowStartPointers[ourMachinePointer] =
				(currentSession.windowStartPointers[ourMachinePointer] + 1)
						% currentSession.windowSize;
	}
	memcpy(data + 12, garbage_data, 1400);
	if (ws.index == currentSession.numberOfPackets)
		type = TYPE_FINALIZE;
	if (!(currentSession.lastSentIndex % 1000)) {
		log_info("sending data message with number %d, clock %d, index %d",
				randomNumber, currentSession.localClock,
				currentSession.lastSentIndex);
//		log_warn(
//				"Total packets sent: %d - retransmissions = %d - polls = %d - feedbacks = %d",
//				currentSession.totalPacketsSent,
//				currentSession.totalRetrasmissions, currentSession.totalPolls,
//				currentSession.totalFeedbacks);

	}

	sendMessage(type, data, 1412);

}

void sendMessage(enum TYPE type, char *dp, int payloadSize) {
	char message[payloadSize + 12];
	memcpy(message, &type, 4);
	memcpy(message + 4, &currentSession.machineIndex, 4);
	u_int32_t lastCounter =
			currentSession.lastDeliveredCounters[currentSession.machineIndex - 1];
	memcpy(message + 8, &lastCounter, 4);
	memcpy(message + 12, dp, payloadSize);
	//busyWait(currentSession.delay);
	currentSession.totalPacketsSent++;
	sendto(currentSession.sendingSocket, &message, payloadSize + 12, 0,
			(struct sockaddr*) &currentSession.sendAddr,
			sizeof(currentSession.sendAddr));

}

void prepareFile() {
	char fileName[6];
	log_info("preparing output file ...");
	sprintf(fileName, "%d.out", currentSession.machineIndex);
	if ((currentSession.f = fopen(fileName, "w")) == NULL) {
		perror("fopen");
		log_fatal("Error opening output file");
		exit(0);
	}
}

void deliverToFile(u_int32_t pid, u_int32_t index, u_int32_t randomData) {
	log_debug("Writing to file");
	fprintf(currentSession.f, "%2d, %8d, %8d\n", pid, index, randomData);
	log_debug("Wrote to file");
	currentSession.lastDeliveredIndexes[pid - 1] = index;
	windowSlot *wsArray = currentSession.dataMatrix[pid - 1];

// if not delivering my own data, move window
	if (pid != currentSession.machineIndex) {
		log_debug("Moving process %d receive window", pid);
		// invalidate the data in the window start
		wsArray[currentSession.windowStartPointers[pid - 1]].valid = 0;
		// move window start pointer
		currentSession.windowStartPointers[pid - 1] =
				(currentSession.windowStartPointers[pid - 1] + 1)
						% currentSession.windowSize;
		log_debug("moved window for process %d. start pointer is %d", pid,
				currentSession.windowStartPointers[pid - 1]);
	} else {
		if (index == currentSession.numberOfPackets) {
			currentSession.fullyDeliveredProcess[pid - 1] = 1;
			currentSession.lastExpectedIndexes[pid - 1] = index;
		}
		synchronizeWindow();
	}

}

void busyWait(u_int32_t loopCount) {
	int i;
	u_int32_t loopVar = loopCount - (rand() % 1000);
	for (i = 0; i < loopVar; i++)
		;
}

void reinitialize() {
	u_int32_t i;
	log_info("reinitializing buffers");

	memset(currentSession.lastDeliveredCounters, 0,
			currentSession.numberOfMachines * sizeof(u_int32_t));
	memset(currentSession.lastInOrderReceivedIndexes, 0,
			currentSession.numberOfMachines * sizeof(u_int32_t));
	memset(currentSession.highestReceivedIndexes, 0,
			currentSession.numberOfMachines * sizeof(u_int32_t));
	memset(currentSession.lastDeliveredIndexes, 0,
			currentSession.numberOfMachines * sizeof(u_int32_t));
	memset(currentSession.fullyDeliveredProcess, 0,
			currentSession.numberOfMachines * sizeof(u_int32_t));
	memset(currentSession.lastExpectedIndexes, 0,
			currentSession.numberOfMachines * sizeof(u_int32_t));
	memset(currentSession.dataMatrix, 0,
			currentSession.numberOfMachines * sizeof(windowSlot*));

	for (i = 0; i < currentSession.numberOfMachines; i++) {
		memset(&currentSession.dataMatrix[i], 0,
				currentSession.windowSize * sizeof(windowSlot));
	}
	currentSession.totalPacketsSent = 0;
	currentSession.localClock = 0;
	currentSession.lastSentIndex = 0;
	currentSession.isFinalDelivery = 0;
	currentSession.lastDeliveredPointer = 0;
	currentSession.exitCounter = 0;
}
