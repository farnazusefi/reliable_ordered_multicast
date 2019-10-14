#include <time.h>
#include "net_include.h"
#include "log.h"

#define TIMEOUT 2000000
#define WINDOW_SIZE 5

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
	//TODO remove garbage if it is not needed!
	u_int32_t *lastInOrderReceivedIndexes;
	u_int32_t *highestReceivedIndexes;
	u_int32_t *windowStartPointers;
	u_int32_t *readyForDelivery;
	u_int32_t *lastDeliveredCounters;
	u_int32_t *finalizedProcessesLastIndices;
	u_int32_t *lastDeliveredIndexes;

	struct timeval *timoutTimestamps;

	u_int32_t isFinalDelivery;
	u_int32_t localClock;
	u_int32_t machineIndex;
	u_int32_t numberOfPackets;
	u_int32_t lossRate;
	u_int32_t windowSize;
	u_int32_t lastSentIndex;
	u_int32_t windowStartIndex;
	u_int32_t lastDeliveredCounter;

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

void parse(void *buf, int bytes);

void handleStartMessage(message *m, int bytes);

void handleDataMessage(void *m, int bytes);

void handleFeedbackMessage(message *m, int bytes);

void handleFinalizeMessage(void *m, int bytes);

void handlePollMessage(void *m, int bytes);

void handleTimeOut(u_int32_t pid);

void startSending();

void synchronizeWindow();

void initializeBuffers();

void initializeAndSendRandomNumber(int moveStartPointer);

void sendMessage(enum TYPE type, char *dp, int payloadSize);

int putInBuffer(dataMessage *m);

void updateLastReceivedIndex(u_int32_t pid);

void checkForDeliveryConditions();

void updateLastDeliveredCounter(u_int32_t pid, u_int32_t lastDeliveredCounter);

void deliverToFile(u_int32_t pid, u_int32_t index, u_int32_t randomData,
		u_int32_t lts);

void sendNack(u_int32_t pid, u_int32_t *indexes, u_int32_t length);

u_int32_t getPointerOfIndex(u_int32_t pid, u_int32_t index);

void resendMessage(u_int32_t index);

session currentSession;

int main(int argc, char **argv) {
	struct sockaddr_in name;

	int mcast_addr;
	struct ip_mreq mreq;
	unsigned char ttl_val;

	fd_set mask;
	fd_set dummy_mask, temp_mask;
	int bytes;
	int num;
	char mess_buf[MAX_MESS_LEN];
	struct timeval timeout;
	int debug_mode = 3;
	mcast_addr = 225 << 24 | 1 << 16 | 3 << 8 | 50; /* (225.1.3.50) */

	if (argc != 5 && argc != 7) {
		printf(
				"Usage: ./mcast <num of packets> <machine index> <num of machines> <loss rate> [delay] [debug mode(1-5)]  \n");
		exit(1);
	}

	// optional args
	if (argc == 7) {
		debug_mode = atoi(argv[6]);
		currentSession.delay = atoi(argv[5]);

		log_info("debug mode = %d, delay = %d \n", debug_mode,
				currentSession.delay);
	}
	log_set_level(debug_mode);
	currentSession.numberOfPackets = atoi(argv[1]);
	currentSession.machineIndex = atoi(argv[2]);
	currentSession.numberOfMachines = atoi(argv[3]);
	currentSession.lossRate = atoi(argv[4]);
	log_info(
			"number of packets = %d, machine index = %d number of machines = %d  loss rate = %d \n",
			currentSession.numberOfPackets, currentSession.machineIndex,
			currentSession.numberOfMachines, currentSession.lossRate);

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
	timeout.tv_sec = 0;
	timeout.tv_usec = 900000;
	initializeBuffers();

	log_trace("test");
	log_debug("test");
	log_info("test");
	log_warn("test");
	log_error("test");
	log_fatal("test");

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
		log_debug("selecting ...");
		num = select(FD_SETSIZE, &temp_mask, &dummy_mask, &dummy_mask,
				&timeout);
		if (num > 0) {
			if (FD_ISSET(currentSession.receivingSocket, &temp_mask)) {
				bytes = recv(currentSession.receivingSocket, mess_buf,
						sizeof(mess_buf), 0); //TODO change to recv_dbg
				mess_buf[bytes] = 0;
				log_debug("received : %s\n", mess_buf);
				parse((void*) mess_buf, bytes);
			}
		} else // timeout for select
		{
			int i;
			log_info("timeout in select. Polling all processes");
			if (currentSession.state == STATE_WAITING)
				continue;
			for (i = 1; i <= currentSession.numberOfMachines; i++) {
				if (i != currentSession.machineIndex)
					handleTimeOut(i);

			}
		}
	}
	return 0;
}

void initializeBuffers() {

	u_int32_t i;
	log_info("initializing buffers");
	currentSession.lastDeliveredCounters = (u_int32_t*) calloc(
			currentSession.numberOfMachines * sizeof(u_int32_t), 0);
	currentSession.lastInOrderReceivedIndexes = (u_int32_t*) calloc(
			currentSession.numberOfMachines * sizeof(u_int32_t), 0);
	currentSession.windowStartPointers = (u_int32_t*) calloc(
			currentSession.numberOfMachines * sizeof(u_int32_t), 0);
	currentSession.readyForDelivery = (u_int32_t*) calloc(
			currentSession.numberOfMachines * sizeof(u_int32_t), 0);
	currentSession.highestReceivedIndexes = (u_int32_t*) calloc(
			currentSession.numberOfMachines * sizeof(u_int32_t), 0);
	currentSession.finalizedProcessesLastIndices = (u_int32_t*) calloc(
			currentSession.numberOfMachines * sizeof(u_int32_t), 0);
	currentSession.lastDeliveredIndexes = (u_int32_t*) calloc(
			currentSession.numberOfMachines * sizeof(u_int32_t), 0);
	currentSession.timoutTimestamps = (struct timeval*) malloc(
			currentSession.numberOfMachines * sizeof(struct timeval));

	currentSession.dataMatrix = (windowSlot**) malloc(
			currentSession.numberOfMachines * sizeof(windowSlot*));

	for (i = 0; i < currentSession.numberOfMachines; i++) {
		currentSession.dataMatrix[i] = (windowSlot*) calloc(
				currentSession.windowSize * sizeof(windowSlot), 0);
	}
	currentSession.localClock = 0;
	currentSession.lastSentIndex = 0;
	currentSession.windowStartIndex = 0;
	currentSession.isFinalDelivery = 0;
	currentSession.lastDeliveredCounter = 0;
	currentSession.windowSize = WINDOW_SIZE;
	srand(time(0));
	prepareFile();
}

void handleTimeOut(u_int32_t pid) {
	char pollMsg[4];
	log_debug("Sending POLL for pid=%d", pid);
	memcpy(pollMsg, &pid, 4);
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
			handleTimeOut(i + 1);
		}
	}
}

void parse(void *buf, int bytes) {
	message *m = (message*) buf;
	if (m->pid == currentSession.machineIndex)
		return;
	log_debug("parsing ...");
	switch (m->type) {
	case TYPE_START:
		handleStartMessage(m, bytes);
		break;
	case TYPE_DATA:
		handleDataMessage(buf, bytes);
		break;
	case TYPE_FEEDBACK:
		handleFeedbackMessage(m, bytes);
		break;
	case TYPE_FINALIZE:
		handleFinalizeMessage(buf, bytes);
		break;
	case TYPE_POLL:
		handlePollMessage(buf, bytes);
		break;
	default:
		log_warn("invalid type %d\n", m->type);
		break;
	}
	if (m->type != TYPE_START) {
		gettimeofday(&currentSession.timoutTimestamps[m->pid - 1], NULL);
		checkTimeoutForOthers();
	}
}

void handlePollMessage(void *m, int bytes) {
	pollMessage *message = (pollMessage*) m;
	u_int32_t polledPid = message->pollPID;
	log_debug("handling poll message from %d for %d", message->pid, polledPid);
	if (polledPid != currentSession.machineIndex)
		return;
	if (currentSession.state == STATE_SENDING)
		resendMessage(currentSession.lastSentIndex);
	else if (currentSession.state == STATE_RECEIVING) {
		char data[1412];
		u_int32_t zero = 0;
		memcpy(data, &zero, 4);
		sendMessage(STATE_FINALIZING, data, 1412);
	}
}

void handleFinalizeMessage(void *m, int bytes) {

	dataMessage *dm = (dataMessage*) m;
	log_debug("received finalize message from %d, with index %d", dm->pid,
			dm->index);
	if (dm->index == 0) {
		currentSession.finalizedProcessesLastIndices[dm->pid - 1] = 1;
		currentSession.lastDeliveredCounters[dm->pid - 1] = 1;
		return;
	}
	currentSession.finalizedProcessesLastIndices[dm->pid - 1] = dm->index;
	handleDataMessage(m, bytes);
}

void resendMessage(u_int32_t index) {
	u_int32_t type = TYPE_DATA;
	windowSlot *myDataArray =
			currentSession.dataMatrix[currentSession.machineIndex - 1];
	windowSlot ws = myDataArray[getPointerOfIndex(currentSession.machineIndex,
			index)];
	char data[1412];
	log_debug("re-sending data index %d", index);

	memcpy(data, &ws.index, 4);
	memcpy(data + 4, &ws.lamportCounter, 4);
	memcpy(data + 8, &ws.randomNumber, 4);

	memcpy(data + 12, garbage_data, 1400);

	if (index == currentSession.numberOfPackets)
		type = TYPE_FINALIZE;
	sendMessage(type, data, 1412);
}

void handleFeedbackMessage(message *m, int bytes) {
	u_int32_t feedBackType = m->data[0];
	u_int32_t lastDeliveredCounter;
	u_int32_t numOfNacks;
	u_int32_t i;
	switch (feedBackType) {
	case FEEDBACK_ACK:
		lastDeliveredCounter = m->data[4];
		log_debug("handling Ack for counter %d from process %d",
				lastDeliveredCounter, m->pid);
		updateLastDeliveredCounter(m->pid, lastDeliveredCounter);
		break;
	case FEEDBACK_NACK:
		if (m->data[4] == currentSession.machineIndex) {
			numOfNacks = m->data[8];
			log_debug("handling Nack for of length %d from process %d",
					numOfNacks, m->pid);
			for (i = 0; i < numOfNacks; i++) {
				u_int32_t index = m->data[4 * (i + 3)];
				resendMessage(index);
			}
		}
		break;
	default:
		break;
	}
}

void updateLastDeliveredCounter(u_int32_t pid, u_int32_t lastDeliveredCounter) {
	if (currentSession.lastDeliveredCounters[pid - 1] > lastDeliveredCounter) {
		currentSession.lastDeliveredCounters[pid - 1] = lastDeliveredCounter;
		synchronizeWindow();
	}
}

void handleDataMessage(void *m, int bytes) {
	dataMessage *dm = (dataMessage*) m;
	log_debug("handling data with counter= %d, index = %d from process %d",
			dm->lamportCounter, dm->index, dm->pid);
	switch (currentSession.state) {
	case STATE_RECEIVING:
	case STATE_SENDING:
		if (dm->lamportCounter > currentSession.localClock)
			currentSession.localClock = dm->lamportCounter;
		if (putInBuffer(dm)) {
			if (dm->index
					> currentSession.lastInOrderReceivedIndexes[dm->pid - 1]) {
				u_int32_t currentPointer = getPointerOfIndex(dm->pid,
						currentSession.lastInOrderReceivedIndexes[dm->pid - 1]);
				u_int32_t lastInOrderIndex =
						currentSession.dataMatrix[dm->pid][currentPointer].index;
				u_int32_t nackIndices[currentSession.windowSize];
				int counter = 0;
				while (currentPointer != getPointerOfIndex(dm->pid, dm->index)) {
					if (currentSession.dataMatrix[dm->pid - 1][currentPointer].index
							< lastInOrderIndex) {
						nackIndices[counter++] =
								currentSession.dataMatrix[dm->pid - 1][currentPointer].index;
					}
					currentPointer = (currentPointer + 1)
							% currentSession.windowSize;
				}
				if (counter > 0)
					sendNack(dm->pid, nackIndices, counter);
			}
		}
		updateLastDeliveredCounter(dm->pid, dm->lastDeliveredCounter);

		break;
	default:
		log_warn("discarding unexpected data");
		break;
	}
}
u_int32_t getMinOfArray(u_int32_t *lastDeliveredCounters) {
	u_int32_t min = -1;
	int i;
	for (i = 0; i < currentSession.numberOfMachines; i++) {
		if (currentSession.lastDeliveredCounters[i] < min)
			min = currentSession.lastDeliveredCounters[i];
	}
	return min;
}

void synchronizeWindow() {
	log_debug("Synchronizing my window");
	u_int32_t minimumOfWindow = getMinOfArray(
			currentSession.lastDeliveredCounters);

	while (1) {
		u_int32_t windowStartPointer =
				currentSession.windowStartPointers[currentSession.machineIndex
						- 1];
		if (currentSession.dataMatrix[currentSession.machineIndex - 1][windowStartPointer].lamportCounter
				<= minimumOfWindow && currentSession.state == STATE_SENDING) {
			initializeAndSendRandomNumber(1);
		} else
			break;
	}

}

void sendNack(u_int32_t pid, u_int32_t *indexes, u_int32_t length) {
	char data[4 + sizeof(u_int32_t) * length];
	u_int32_t feedbackType = FEEDBACK_NACK;
	memcpy(data, &feedbackType, 4);
	memcpy(data + 4, &pid, 4);
	memcpy(data + 8, &length, 4);
	int i;
	for (i = 1; i <= length; i++) {
		data[(4 * i) + 8] = indexes[i - 1];
	}

	sendMessage(TYPE_FEEDBACK, data, 4 + sizeof(u_int32_t) * length);
}

u_int32_t getPointerOfIndex(u_int32_t pid, u_int32_t index) {
	windowSlot *currentWindowSlot = currentSession.dataMatrix[pid - 1];
	u_int32_t currentWindowStartPointer = currentSession.windowStartPointers[pid
			- 1];

	return (currentWindowStartPointer
			+ (index - currentWindowSlot[currentWindowStartPointer].index)-1)
			% currentSession.windowSize;
}

int putInBuffer(dataMessage *m) {

	windowSlot *currentWindowSlot = currentSession.dataMatrix[m->pid - 1];
	windowSlot ws;
	u_int32_t currentWindowStartPointer =
			currentSession.windowStartPointers[m->pid - 1];
	u_int32_t startIndex = currentWindowSlot[currentWindowStartPointer].index;
	log_debug("putInBuffer Condition Check, last inorder received idx = %d, startIndex = %d", currentSession.lastInOrderReceivedIndexes[m->pid - 1], startIndex);
	// Check if the received packet's index is in the valid range for me to store
	if (m->index > currentSession.lastInOrderReceivedIndexes[m->pid - 1]
			&& m->index < (startIndex + currentSession.windowSize)) {
		log_debug("putting in buffer, counter %d, index %d from process %d",
				m->lamportCounter, m->index, m->pid);
		ws.index = m->index;
		ws.lamportCounter = m->lamportCounter;
		ws.randomNumber = m->randomNumber;
		ws.valid = 1;
		currentWindowSlot[getPointerOfIndex(m->pid, m->index)] = ws;
		updateLastReceivedIndex(m->pid);
		checkForDeliveryConditions(m->lamportCounter);
		return 1;
	}
	log_debug(
			"not putting in buffer (retransmitted data), counter %d, index %d from process %d",
			m->lamportCounter, m->index, m->pid);
	return 0;
}

int dataRemaining() {
	int i;
	for (i = 0; i < currentSession.numberOfMachines; i++)
		if (currentSession.readyForDelivery)
			return 1;
	return 0;
}

void deliverLowestData() {
	int i;
	u_int32_t minimumClock = -1;
	u_int32_t minimumPID, minimumIndex, randomData;
	for (i = 0; i < currentSession.numberOfMachines; i++) {
		if (currentSession.readyForDelivery[i])
			continue;
		if (currentSession.dataMatrix[i][currentSession.windowStartPointers[i]].lamportCounter
				< minimumClock) {
			minimumClock =
					currentSession.dataMatrix[i][currentSession.windowStartPointers[i]].lamportCounter;
			minimumPID = i;
			minimumIndex =
					currentSession.dataMatrix[i][currentSession.windowStartPointers[i]].index;
			randomData =
					currentSession.dataMatrix[i][currentSession.windowStartPointers[i]].randomNumber;
		}
	}
	log_debug(
			"delivering to file, counter %d, index %d from process %d, data: %d",
			minimumClock, minimumIndex, minimumPID, randomData);

	deliverToFile(minimumPID, minimumIndex, randomData, minimumClock);
}

void checkTermination() {
	log_debug("checking termination conditions");
	if (getMinOfArray(currentSession.finalizedProcessesLastIndices) != 0) {
		checkForDeliveryConditions(0);
		int i;
		for (i = 0; i < currentSession.numberOfMachines; i++) {
			if (currentSession.finalizedProcessesLastIndices[i]
					!= currentSession.lastDeliveredIndexes[i])
				return;
		}
		log_info("termination conditions hold. Exiting, BYE!");
		fclose(currentSession.f);
		exit(0);
	}
}

void checkForDeliveryConditions(u_int32_t receivedCounter) {
	log_debug("checking delivery conditions");
	if (receivedCounter == 0) {
		int i;
		for (i = 0; i < currentSession.numberOfMachines; i++) {
			if (currentSession.lastInOrderReceivedIndexes[i]
					!= currentSession.finalizedProcessesLastIndices[i]) {
				return;
			}
		}
		currentSession.isFinalDelivery = 1;
		for (i = 0; i < currentSession.numberOfMachines; i++) {
			if (currentSession.lastDeliveredIndexes[i]
					!= currentSession.finalizedProcessesLastIndices[i]) {
				currentSession.readyForDelivery[i] = 1;
			}
		}
		while (dataRemaining())
			deliverLowestData();

		return;
	}
	if ((receivedCounter - currentSession.lastDeliveredCounter) < 2)
		return;
	int i;
	for (i = 0; i < currentSession.numberOfMachines; i++)
		if (!currentSession.readyForDelivery[i])
			return; // TODO: Maybe POLL process here
	while (dataRemaining()) {
		deliverLowestData();
	}
	if (currentSession.numberOfPackets == 0) {
		char data[8];
		u_int32_t feedbackType = FEEDBACK_ACK;
		memcpy(data, &feedbackType, 4);
		memcpy(data + 4, &currentSession.lastDeliveredCounter, 4);
		log_debug("Acknowledging data for clock %d",
				currentSession.lastDeliveredCounter);
		sendMessage(TYPE_FEEDBACK, data, 8);
	}
	checkTermination();
}

void updateLastReceivedIndex(u_int32_t pid) {

	windowSlot *currentWindowSlot = currentSession.dataMatrix[pid - 1];
	u_int32_t lastValidIndex =
			currentSession.lastInOrderReceivedIndexes[pid - 1];
	u_int32_t windowStartPointer = currentSession.windowStartPointers[pid - 1];
	u_int32_t lastValidIndexPointer = getPointerOfIndex(pid, lastValidIndex);
	log_debug("Updating last received index");

	if (lastValidIndex == 0) {
		currentWindowSlot[lastValidIndexPointer].valid = 1;
		currentSession.lastInOrderReceivedIndexes[pid - 1] = 1;
		return;
	}
	u_int32_t searchingPointer = (lastValidIndexPointer + 1)
			% currentSession.windowSize;
	while (searchingPointer != windowStartPointer) {
		if (!currentWindowSlot[searchingPointer].valid)
			break;
		currentSession.lastInOrderReceivedIndexes[pid - 1]++;
		searchingPointer = (searchingPointer + 1) % currentSession.windowSize;
		currentSession.readyForDelivery[pid - 1] = 1;

	}

}

void handleStartMessage(message *m, int bytes) {
	struct timeval t;
	int i;
	log_info("handling start message. Starting ...");
	switch (currentSession.state) {
	case STATE_WAITING:
		gettimeofday(&t, NULL);
		for (i = 0; i < currentSession.numberOfMachines; i++) {
			currentSession.timoutTimestamps[i] = t;
		}
		if (currentSession.numberOfPackets > 0) {
			currentSession.state = STATE_SENDING;
			startSending();
		} else {
			currentSession.state = STATE_RECEIVING;
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
		initializeAndSendRandomNumber(0);
	}
}

void initializeAndSendRandomNumber(int moveStartpointer) {
	windowSlot ws;
	u_int32_t type = TYPE_DATA;
	u_int32_t randomNumber = rand();
	char data[1412];
	++currentSession.lastSentIndex;
	++currentSession.localClock;
	memcpy(data, &currentSession.lastSentIndex, 4);
	memcpy(data + 4, &currentSession.localClock, 4);
	memcpy(data + 8, &randomNumber, 4);
	ws.index = currentSession.lastSentIndex;
	ws.lamportCounter = currentSession.localClock;
	ws.randomNumber = randomNumber;
	currentSession.dataMatrix[currentSession.machineIndex - 1][currentSession.windowStartPointers[currentSession.machineIndex
			- 1]] = ws;
	if (moveStartpointer)
		currentSession.windowStartPointers[currentSession.machineIndex - 1]++;
	memcpy(data + 12, garbage_data, 1400);
	if (ws.index == currentSession.numberOfPackets)
		type = TYPE_FINALIZE;
	log_debug("sending data message with number %d, clock %d, index %d",
			randomNumber, currentSession.localClock,
			currentSession.lastSentIndex);
	sendMessage(type, data, 1412);

}

void sendMessage(enum TYPE type, char *dp, int payloadSize) {
	char message[payloadSize + 12];
	memcpy(message, &type, 4);
	memcpy(message + 4, &currentSession.machineIndex, 4);
	memcpy(message + 8,
			&currentSession.lastDeliveredCounters[currentSession.machineIndex
					- 1], 4);
	memcpy(message + 12, dp, payloadSize);
	log_trace("sending:");
	int j;
	for (j = 0; j < 12; j++)
		log_trace("%02X", message[j]);
	log_trace("\n");
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

void deliverToFile(u_int32_t pid, u_int32_t index, u_int32_t randomData,
		u_int32_t lts) {
	fprintf(currentSession.f, "%2d, %8d, %8d\n", pid, index, randomData);
	currentSession.lastDeliveredCounter = lts;
	currentSession.lastDeliveredCounters[currentSession.machineIndex - 1] = lts;
	currentSession.lastDeliveredIndexes[pid - 1] = index;
	windowSlot *wsArray = currentSession.dataMatrix[pid - 1];

	if (pid != currentSession.machineIndex) {
		wsArray[currentSession.windowStartPointers[pid - 1]].valid = 0;
		currentSession.windowStartPointers[pid - 1] =
				(currentSession.windowStartPointers[pid - 1] + 1)
						% currentSession.windowSize;
		u_int32_t currentIndex = wsArray[currentSession.windowStartPointers[pid
				- 1]].index;
		currentSession.readyForDelivery[pid - 1] = 0;

		while (currentIndex
				!= currentSession.lastInOrderReceivedIndexes[pid - 1]) {
			if (wsArray[currentIndex].lamportCounter
					<= currentSession.localClock - 1
					|| currentSession.isFinalDelivery) {
				currentSession.readyForDelivery[pid - 1] = 1;
				break;
			}
		}
	}

}
