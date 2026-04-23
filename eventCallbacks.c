#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "library.h"
#include "global.h"

/*
 * SECTION 1: GLOBAL DATA
 */

static long timeout_ns; //
static uint32_t sender_seq_next;        
static uint32_t receiver_seq_expected;
static int waiting_ack;  
static packet_t *lst_packet_buffer;
static size_t last_packet_len; 
static const int retransmission_timer_id = 0;

/*
 * SECTION 2: CALLBACK FUNCTIONS
 */

void connection_initialization(int _windowSize, long timeout_in_ns) {
    timeout_ns = timeout_in_ns; //timeout value
    sender_seq_next = 1; //initialize to 1 
    receiver_seq_expected = 1; //initialize to 1
    waiting_ack = 0;
    lst_packet_buffer  = (packet_t *)malloc(sizeof(packet_t)); //initialize in the memory the last sent packet
    if (lst_packet_buffer == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    last_packet_len = 0;
    printf("Protocol Stop-and-Wait started. Timeout: %ld ns.\n", timeout_ns);
}



/* Callback when a packet pkt of size n is received*/
void receive_callback(packet_t *pkt, size_t n) {
    DEBUG_RECEPTION(2, "Packet received. Type: %d", pkt->type);

    if (!VALIDATE_CHECKSUM(pkt)) {
        printf("Corrupted packet received, discarded.\n");
        return;
    }

    if (pkt->type == DATA){ // data packet case
        DEBUG_RECEPTION(1, "Data packet received, SEQ index %d", pkt->seqno);
        
        if (pkt->seqno == receiver_seq_expected) { // expected packet case
            printf("Packet to %d received: ", pkt->seqno); 
            
            ACCEPT_DATA(pkt->data, pkt->len - 10); // accept data
            
            SEND_ACK_PACKET(receiver_seq_expected); // send ACK  
            
            printf("Ack (acko) %d sent.\n", receiver_seq_expected);

            sender_seq_next=(sender_seq_next+1)%2;  // update sequence number
        }
        else if (pkt->seqno < receiver_seq_expected) { // duplicate packet case
            printf("Duplicate packet %d received, resending ACK %d.\n", pkt->seqno, pkt->seqno);
            SEND_ACK_PACKET(pkt->seqno); // resend ACK for duplicate
        }
        else {
            printf("Out of order packet %d received (expected %d). Discarded.\n", pkt->seqno, receiver_seq_expected);
        }
    }
    else if (pkt->type == ACK){ // ACK packet case
        DEBUG_RECEPTION(2, "ACK packet received, ACK index %d", pkt->ackno);
        
        if (waiting_ack && pkt->ackno == (u_int16_t)sender_seq_next) { // expected ACK case
            printf("Ack (acki) %d received.\n", pkt->ackno);

            CLEAR_TIMER(retransmission_timer_id); // clear retransmission timer
            waiting_ack = 0; // clear waiting for ACK flag
            sender_seq_next=(sender_seq_next+1)%2; // update sequence number
            
            RESUME_TRANSMISSION();
        }
        else if ((waiting_ack && pkt->ackno)+1 < (int16_t)sender_seq_next) { // duplicate ACK case
            printf("Duplicate Ack %d received (expected %d). Ignored.\n", pkt->ackno, sender_seq_next);
        }
        else {
            printf("Unexpected Ack %d received. Ignored.\n", pkt->ackno);
        }
    }   
}


/* Callback when the application wants to send data to the other end*/
void send_callback() {
    DEBUG_SEND(2, "send_callback called.");
    // Check if we are waiting for an ACK
    if (waiting_ack) {
        printf("Waiting for ACK %d, cannot send new data.\n", sender_seq_next);
        PAUSE_TRANSMISSION();   //Pause transmission until ACK is received
        return;
    }
    // Read data from application layer
    int data_len = READ_DATA_FROM_APP_LAYER(lst_packet_buffer->data, 500); // read data from application layer
    if (data_len <= 0){ // If the length is 0, no data available (or error)
        DEBUG_SEND(3, " No data available from application layer.");
        return; 
    }
    
    // Prepare packet
    last_packet_len = data_len + 10; // 10 byte for the header 
    lst_packet_buffer->type = DATA; 
    lst_packet_buffer->len = (uint16_t) last_packet_len;
    lst_packet_buffer->seqno = (int16_t) sender_seq_next;
    lst_packet_buffer->ackno = 0; // not used in data packets
    
    // Send packet
    printf("Sending Packet (ti) %d...\n", sender_seq_next);
    SEND_DATA_PACKET(lst_packet_buffer->type, lst_packet_buffer->len, lst_packet_buffer->ackno, lst_packet_buffer->seqno, lst_packet_buffer->data);

    // Update state
    waiting_ack = 1;
     
    // Start timer for retransmission
    SET_TIMER(retransmission_timer_id, timeout_ns);
}

/*
 * Function timer with index "timerNumber" expires.
 */
void timer_callback(int timerNumber) {
    if(timerNumber == retransmission_timer_id) {
        DEBUG_TIMER(1, "Timer %d expired", timerNumber); 
        if (waiting_ack){ // only retransmit if still waiting for ACK
            // Event 'to' (Timeout)
            printf("Timeout! Retransmitting Packet %d...\n", lst_packet_buffer->seqno);
            
            // Resend packet
            SEND_DATA_PACKET(lst_packet_buffer->type, lst_packet_buffer->len, lst_packet_buffer->ackno, lst_packet_buffer->seqno, lst_packet_buffer->data);
            SET_TIMER(retransmission_timer_id, timeout_ns); // restart timer
        }
        else {
            DEBUG_TIMER(2, "No retransmission needed, ACK already received.");
        }
    }    
}