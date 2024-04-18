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
    
    //sequence numbers to keep track off
    uint32_t recv_ackno; // which ackno has been received
    uint32_t send_ackno; // which seqno is the next ackno

    uint32_t next_output_seqno; //next sequence number to output
    uint32_t next_input_seqno; //next sequence number for a package created from input
    // flags to keep track of the conditions to close the session
    int recv_eof;
    int read_eof;
    int all_packs_acked;
    int all_output_written;
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
    
    r->recv_ackno = 1;
    r->send_ackno = 1;

    r->next_output_seqno = 1;
    r->next_input_seqno = 1;

    //init flags to 0
    r->all_output_written = 1;
    r->all_packs_acked = 1;
    r->read_eof = 0;
    r->recv_eof = 0;

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
    

    fprintf(stderr, "reliable protocol session destoyed \n");

}
int check_rel_destory(rel_t *r){
    if(r->all_output_written && r->all_packs_acked && r->read_eof && r->recv_eof){
        rel_destroy(r);
        return 1;
    }
    return 0;
}

void update_send_ackno(rel_t * r){
    // function to find the next in order sequence number

    buffer_node_t *current = r->rec_buffer->head;
    if(current == NULL){
        // buffer is empty
        return;
    }
    while(current->next != NULL && ntohl(current->packet.seqno) == ntohl(current->next->packet.seqno)-1){
        current = current->next;
    }
    r->send_ackno = ntohl(current->packet.seqno) + 1;
    fprintf(stderr, "next ackno to send calculated: %d \n", r->send_ackno);
}

void send_ack(rel_t *r){
    packet_t ack_pkt;
    memset(&ack_pkt, 0, sizeof(ack_pkt));
    ack_pkt.ackno = htonl(r->send_ackno); 
    ack_pkt.len = htons(8); // ACK packet size
    ack_pkt.cksum = 0;
    ack_pkt.cksum = cksum(&ack_pkt, 8);

    conn_sendpkt(r->c, &ack_pkt, 8);
    fprintf(stderr, "Sent an ack with ackno %d \n", ntohl(ack_pkt.ackno));
}

// n is the expected length of pkt
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
    
   fprintf(stderr, "recv packet called \n");
    
    if (n < 8 || n > 512) { 
        fprintf(stderr, "invalid packet length \n");
        return; // Packet is corrupted or incomplete
    }

    // checksum
    uint16_t pkt_cksum = pkt->cksum;
    pkt->cksum = 0;
    if (cksum(pkt, n) != pkt_cksum) {
        fprintf(stderr, "Checksum did not match \n");
        return; // Checksum does not match, packet is corrupted
    }

    // ack packets
    if (n == 8) {
        
        fprintf(stderr, "Received an ack with ackno %d\n", ntohl(pkt->ackno));
        fprintf(stderr, "Ackno received so far %d\n", r->recv_ackno);
        
        //Check if the sliding window has moved
        if(ntohl(pkt->ackno)>r->recv_ackno){
            fprintf(stderr, "Sliding window has changed \n");
            r->recv_ackno =  ntohl(pkt->ackno);
            rel_read(r);
            buffer_remove(r->send_buffer, r->recv_ackno);
            fprintf(stderr, "removed packets up to %d from buffer \n", r->recv_ackno);
        }

        // check if send buffer is now empty
        if(r->send_buffer->head == NULL){
            fprintf(stderr, "send buffer is now empty \n");
            r->all_packs_acked = 1;
            return;
        }
        return;
    }

    // received an eof
    if(n == 12){
        fprintf(stderr, "received EOF\n");
        r->recv_eof = 1;
        //not sure if we need to ack eofs
        send_ack(r);
        return;
    }

    // buffer does not contain the data
    if (!buffer_contains(r->rec_buffer, ntohl(pkt->seqno))) {
        // insert packet into buffer
        struct timeval now;
        gettimeofday(&now, NULL);
        long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;

        buffer_insert(r->rec_buffer, pkt, now_ms);
        r->all_output_written = 0;
        update_send_ackno(r);
        fprintf(stderr, "Inserted packet into buffer\n");
        
    }else{
        fprintf(stderr, "received a packet that was already in buffer\n");
    }
    send_ack(r);
    
}


void rel_read(rel_t *r) {
    // Check if the send window is full
    if ((r->next_input_seqno - (r->recv_ackno-1)) > r->window) {
        fprintf(stderr, "Max window is: %d, seqno of next packet is: %d, received in order ack up to packet: %d \n", r->window, r->next_input_seqno, (r->recv_ackno-1));
        return; // Window is full, cannot send more data yet
    }

    // Read data and prepare data packet
    char data[500];
    int data_len = conn_input(r->c, data, sizeof(data));
    if (data_len <= 0) {
        r->read_eof = 1;
        fprintf(stderr, "no data read or EOF/error\n");
        return; // No data read or EOF/error
    }
    
    // Create and send packet
    packet_t pkt;
    void * pkt_pointer = memset(&pkt, 0, sizeof(pkt));
    pkt.seqno = htonl(r->next_input_seqno); // Next sequence number
    pkt.cksum = 0;
    pkt.len = htons(12 + data_len);
    memcpy(pkt.data, data, data_len);
    pkt.cksum = cksum(&pkt, 12 + data_len);

    if (conn_sendpkt(r->c, &pkt, 12 + data_len) > 0) {
        // Successfully sent, insert into send buffer for potential retransmission
        r->next_input_seqno++;
        struct timeval now;
        gettimeofday(&now, NULL);
        long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;

        buffer_insert(r->send_buffer, &pkt, now_ms);
        r->all_packs_acked = 0;
    }else{
        free(pkt_pointer);
    }
    return;
}


void rel_output(rel_t *r) {
    // fprintf(stderr, "rel_output called\n");
    buffer_node_t *node = buffer_get_first(r->rec_buffer);
    while (node != NULL) {
        
        //ensure the sequence number is correct
        if(ntohl(node->packet.seqno) != r->next_output_seqno){
            break;
        }
        // Calculate data length excluding header
        size_t data_len = ntohs(node->packet.len) - 12;
        if (conn_bufspace(r->c) < data_len) {
            // Not enough space in output buffer, exit to avoid partial writes
            return;
        }
        conn_output(r->c, node->packet.data, data_len);
        // update next seqno to output
        r->next_output_seqno++;
        // Move to next packet after successful write
        buffer_remove_first(r->rec_buffer);
        node = buffer_get_first(r->rec_buffer);
    }
    if(node == NULL){
        //fprintf(stderr, "all data output written\n");
        r->all_output_written = 1;
    }

}



void rel_timer() {

    for (rel_t *current = rel_list; current != NULL; current = current->next) {
        
        //fprintf(stderr, "Received EOF %d,Read EOF %d, all packets acked %d, all output written %d \n", current->recv_eof, current->read_eof, current->all_packs_acked, current->all_output_written);
        if(check_rel_destory(current)){
            continue;
        }

        //call output
        rel_output(current);

        // Check and retransmit any packets that have timed out
        struct timeval now;
        gettimeofday(&now, NULL);
        long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;
        
        for (buffer_node_t *node = buffer_get_first(current->send_buffer);
             node != NULL; node = node->next) {
            if (now_ms - node->last_retransmit > current->timeout) {
                // Retransmit packet
                node->packet.cksum = 0; // Reset checksum
                node->packet.cksum = cksum(&node->packet, ntohs(node->packet.len));
                conn_sendpkt(current->c, &node->packet, ntohs(node->packet.len));
                node->last_retransmit = now_ms;
                fprintf(stderr,"packet has been resent \n");
            }
             }

    }
}





