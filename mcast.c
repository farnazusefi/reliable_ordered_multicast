#include "net_include.h"

void parse(void *buf, int bytes);

void handleStartMessage(message *m, int bytes);

void handleDataMessage(message *m, int bytes);

void handleFeedbackMessage(message *m, int bytes);

void handleFinalizeMessage(message *m, int bytes);

void handlePollMessage(message *m, int bytes);

void handleTimeOut();

typedef struct messageT {
    u_int32_t type;
    u_int32_t pid;
    void *data;
} message;

typedef struct dataPayloadT {
    u_int32_t lamportCounter;
    u_int32_t randomNumber;
    u_int32_t index;
    char *garbage;
} dataPayload;
//TODO Struct feedback payload
enum STATE {
    STATE_WAITING,
    STATE_SENDING, // sending and receiving
    STATE_RECEIVING,
    STATE_FINALIZING
};

enum TYPE {
    TYPE_START,
    TYPE_DATA,
    TYPE_FEEDBACK,
    TYPE_POLL,
    TYPE_FINALIZE
};

typedef struct sessionT {
    enum STATE state;
    int num_participants;
    int sender;
    dataPayload **dataMatrix;
    //TODO remove garbage if it is not needed!
    u_int32_t *lastReceivedIndexes;
    u_int32_t localClock;
    u_int32_t machineIndex;
    u_int32_t numberOfPackets;
    u_int32_t lossRate;


} session;

typedef struct fileToReceiveT {
    int fileDescriptor;
    FILE *fw; /* Pointer to dest file, which we write  */
    unsigned long totalLinesWritten;
} fileToReceive;

int main() {
    struct sockaddr_in name;
    struct sockaddr_in send_addr;

    int mcast_addr;

    struct ip_mreq mreq;
    unsigned char ttl_val;

    int ss, sr;
    fd_set mask;
    fd_set dummy_mask, temp_mask;
    int bytes;
    int num;
    char mess_buf[MAX_MESS_LEN];
    char input_buf[80];
    struct timeval timeout;

    mcast_addr = 225 << 24 | 1 << 16 | 3 << 8 | 50; /* (225.1.3.50) */

    sr = socket(AF_INET, SOCK_DGRAM, 0); /* socket for receiving */
    if (sr < 0) {
        perror("Mcast: socket");
        exit(1);
    }

    name.sin_family = AF_INET;
    name.sin_addr.s_addr = INADDR_ANY;
    name.sin_port = htons(PORT);

    if (bind(sr, (struct sockaddr *) &name, sizeof(name)) < 0) {
        perror("Mcast: bind");
        exit(1);
    }

    mreq.imr_multiaddr.s_addr = htonl(mcast_addr);

    /* the interface could be changed to a specific interface if needed */
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(sr, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void *) &mreq,
                   sizeof(mreq)) < 0) {
        perror("Mcast: problem in setsockopt to join multicast address");
    }

    ss = socket(AF_INET, SOCK_DGRAM, 0); /* Socket for sending */
    if (ss < 0) {
        perror("Mcast: socket");
        exit(1);
    }

    ttl_val = 1;
    if (setsockopt(ss, IPPROTO_IP, IP_MULTICAST_TTL, (void *) &ttl_val,
                   sizeof(ttl_val)) < 0) {
        printf("Mcast: problem in setsockopt of multicast ttl %d - ignore in WinNT or Win95\n", ttl_val);
    }

    send_addr.sin_family = AF_INET;
    send_addr.sin_addr.s_addr = htonl(mcast_addr);  /* mcast address */
    send_addr.sin_port = htons(PORT);

    timeout.tv_sec = 0;
    timeout.tv_usec = 800;

    FD_ZERO(&mask);
    FD_ZERO(&dummy_mask);
    FD_SET(sr, &mask);
    FD_SET((long) 0, &mask);    /* stdin */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"
    for (;;) {
        temp_mask = mask;
        num = select(FD_SETSIZE, &temp_mask, &dummy_mask, &dummy_mask, &timeout);
        if (num > 0) {
            if (FD_ISSET(sr, &temp_mask)) {
                bytes = recv(sr, mess_buf, sizeof(mess_buf), 0);
                mess_buf[bytes] = 0;
                printf("received : %s\n", mess_buf);
                parse((void*)mess_buf, bytes);

            } else if (FD_ISSET(0, &temp_mask)) {
                bytes = read(0, input_buf, sizeof(input_buf));
                input_buf[bytes] = 0;
                printf("there is an input: %s\n", input_buf);
                sendto(ss, input_buf, strlen(input_buf), 0,
                       (struct sockaddr *) &send_addr, sizeof(send_addr));
            }
        }
        else // timeout for select
        {
            handleTimeOut();
        }
    }
#pragma clang diagnostic pop

    return 0;
}

void handleTimeOut() {

}

void parse(void *buf, int bytes) {

    message* m = (message*) buf;
    switch (m->type){
        case TYPE_START:
            handleStartMessage(m, bytes);
            break;
        case TYPE_DATA:
            handleDataMessage(m, bytes);
            break;
        case TYPE_FEEDBACK:
            handleFeedbackMessage(m, bytes);
            break;
        case TYPE_FINALIZE:
            handleFinalizeMessage(m, bytes);
            break;
        case TYPE_POLL:
            handlePollMessage(m, bytes);
            break;
        default:
            printf("invalid type %d\n",m->type);
            break;
    }
}

void handlePollMessage(message *m, int bytes) {

}

void handleFinalizeMessage(message *m, int bytes) {

}

void handleFeedbackMessage(message *m, int bytes) {

}

void handleDataMessage(message *m, int bytes) {

}

void handleStartMessage(message *m, int bytes) {

}
void prepareFile(){


}
void deliverToFile(){

}