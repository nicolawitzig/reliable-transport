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

#include "rlib.h"
#include "buffer.h"

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    /* Add your own data fields below this */
    // ...
    buffer_t* send_buffer;
    // ...
    buffer_t* rec_buffer;
    // ...
    // fields added
    int window;			/* # of unacknowledged packets in flight */
    int timer;			/* How often rel_timer called in milliseconds */
    int timeout;			/* Retransmission timeout in milliseconds */

    // array to keep track which seq_no the buffer has already seen
    char* inserted_packets;
    // array to keep track of the conditions to close the session
    char* ready_to_close;
};

rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;

    /* Do any other initialization you need here... */
    // ...
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // ...
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;
    // ...
    // initialize added fields
    r->window = cc->window;
    r->timer = cc->timer;
    r->timeout = cc->timeout;

    r->inserted_packets = (char*)calloc(4, sizeof(char));
    r->ready_to_close = (char*)calloc(4, sizeof(char));

    fprintf(stderr, "reliable protocol session created \n");

    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...
    free(r->ready_to_close);
    free(r->inserted_packets);

    fprintf(stderr, "reliable protocol session destoyed \n");

}
void check_rel_destory(rel_t *r){
    if((r->ready_to_close[0])==(r->ready_to_close[1])==(r->ready_to_close[2])==(r->ready_to_close[3])==1){
                rel_destroy(r);
                fprintf(stderr, "Session was destroyed");
            }
    return;
}

long buffer_next_seqno(buffer_t *buffer){
    // function to find the next in order sequence number

    buffer_node_t *current = buffer->head;
    if(current == NULL){
        // buffer is empty
        return 1;
    }
    while(current->next != NULL && ntohl(current->packet.seqno) == ntohl(current->next->packet.seqno)-1){
        current = current->next;
    }
    fprintf(stderr, "next sequence number calculated \n");

    return ntohl(current->packet.seqno) + 1;
}

void send_ack(rel_t *r){
    packet_t ack_pkt;
    memset(&ack_pkt, 0, sizeof(ack_pkt));
    ack_pkt.ackno = htonl(buffer_next_seqno(r->rec_buffer)); // Next expected seqno
    ack_pkt.len = htons(8); // ACK packet size
    ack_pkt.cksum = cksum(&ack_pkt, 8);

    conn_sendpkt(r->c, &ack_pkt, 8);
    fprintf(stderr, "Sent an ack \n");
}

// n is the expected length of pkt
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
    fprintf(stderr, "recv packet called \n");
    if (n < 8) { // Minimum packet size check (ACK packet size)
        fprintf(stderr, "Packet is corrupted or incomplete \n");
        return; // Packet is corrupted or incomplete
    }

    // Verify checksum
    uint16_t pkt_cksum = pkt->cksum;
    pkt->cksum = 0;
    if (cksum(pkt, n) != pkt_cksum) {
        fprintf(stderr, "Checksum did not match \n");
        return; // Checksum does not match, packet is corrupted
    }

    // Handle ACK packets
    if (n == 8) {
        // Process ACK, remove acknowledged packets from send buffer
        fprintf(stderr, "Received an ack \n");
        buffer_remove(r->send_buffer, ntohl(pkt->ackno));
        fprintf(stderr, "removed packets up to achno from buffer \n");
        if(r->send_buffer->head == NULL){
            fprintf(stderr, "send buffer is now empty \n");
            // send buffer is now empty
            r->ready_to_close[2] = 1;
            check_rel_destory(r);
            
            return;
        }
        fprintf(stderr, "send buffer is not empty \n");
        return;
    }

    // EOF
    if(n == 12){
        fprintf(stderr, "all received EOF");
        r->ready_to_close[0] = 1;
        check_rel_destory(r);
    }

    // Non empty data packet
    // buffer does not contain the data
    if (!buffer_contains(r->rec_buffer, ntohl(pkt->seqno))) {
        // insert packet into buffer
        buffer_insert(r->rec_buffer, pkt, (long)time(NULL));
        fprintf(stderr, "Inserted packet into buffer\n");
    }
    send_ack(r);
    
}


void rel_read(rel_t *r) {

    // Check if the send window is full
    if (buffer_size(r->send_buffer) >= r->window) {
        fprintf(stderr, "send window is full\n");
        return; // Window is full, cannot send more data yet
    }
    

    // Read data and prepare data packet
    char data[500];
    int data_len = conn_input(r->c, data, sizeof(data));
    if (data_len <= 0) {
        r->ready_to_close[1] = 1;
        fprintf(stderr, "no data read or EOF/error");
        check_rel_destory(r);
        return; // No data read or EOF/error
    }
    

    // Create and send packet
    packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.seqno = htonl(buffer_next_seqno(r->send_buffer)); // Next sequence number
    pkt.len = htons(12 + data_len);
    memcpy(pkt.data, data, data_len);
    pkt.cksum = cksum(&pkt, 12 + data_len);

    if (conn_sendpkt(r->c, &pkt, 12 + data_len) > 0) {
        // Successfully sent, insert into send buffer for potential retransmission
        buffer_insert(r->send_buffer, &pkt, (long)time(NULL));
    }

    return;
}


void rel_output(rel_t *r) {
    buffer_node_t *node = buffer_get_first(r->rec_buffer);
    while (node != NULL) {
        // Ensure packet length is at least the size of the header
        if (ntohs(node->packet.len) < 12) {
            // Log or handle error: Invalid packet size
            break;
        }

        // Calculate data length excluding header
        size_t data_len = ntohs(node->packet.len) - 12;
        if (conn_bufspace(r->c) < data_len) {
            // Not enough space in output buffer, exit to avoid partial writes
            return;
        }

        // Safeguard against writing 0 data (could be extended for other control logic)
        if (data_len > 0) {
            conn_output(r->c, node->packet.data, data_len);
        }

        // Move to next packet after successful write
        buffer_remove_first(r->rec_buffer);
        node = buffer_get_first(r->rec_buffer);
    }
    fprintf(stderr, "all data output written");
    r->ready_to_close[3] = 1;
    check_rel_destory(r);

}



void rel_timer() {
    for (rel_t *current = rel_list; current != NULL; current = current->next) {
        // Check and retransmit any packets that have timed out
        long current_time = (long)time(NULL);
        for (buffer_node_t *node = buffer_get_first(current->send_buffer);
             node != NULL; node = node->next) {
            if (current_time - node->last_retransmit > current->timeout) {
                // Retransmit packet
                node->packet.cksum = 0; // Reset checksum
                node->packet.cksum = cksum(&node->packet, ntohs(node->packet.len));
                conn_sendpkt(current->c, &node->packet, ntohs(node->packet.len));
                node->last_retransmit = current_time;
                fprintf(stderr,"packet has been resent");
            }
        }
    }
}





