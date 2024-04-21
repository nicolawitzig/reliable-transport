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

    int send_sliding_window_start;
    int send_max_window_size;
    int send_next_not_alloc;
    int send_eof;

    int rec_sliding_window_start;
    int rec_window_size;
    int rec_max_window_size;
    int rec_eof;
    int rec_ackno;

    long timeout;

    buffer_t* send_buffer;
    buffer_t* rec_buffer;

};
rel_t *rel_list;


packet_t* rel_make_data_pkt(uint16_t n, uint32_t seqno, char data[500], uint32_t ackno){
    // creates a data packet from the params
    packet_t *pkt = xmalloc (sizeof (packet_t));
    pkt->ackno = htonl(ackno);
    pkt->cksum = htons(0);
    pkt->len = htons(n + 12);
    pkt->seqno = htonl(seqno);
    memcpy(pkt->data, data, 500);
    pkt->cksum = cksum(pkt, n + 12);
    return pkt;
}

packet_t* rel_make_eof_pkt(uint32_t seqno, uint32_t ackno){
    // creates a data packet from the params
    packet_t *pkt = xmalloc (sizeof (packet_t));
    pkt->ackno = htonl(ackno);
    pkt->cksum = htons(0);
    pkt->len = htons(12);
    pkt->seqno = htonl(seqno);
    pkt->cksum = cksum(pkt, 12);
    return pkt;
}

packet_t* rel_make_ack_pkt(uint32_t ackno){
    // creates a ack packet from the params
    packet_t *pkt = xmalloc (sizeof (packet_t));
    pkt->ackno = htonl(ackno);
    pkt->cksum = htons(0);
    pkt->len = htons(8);
    pkt->cksum = cksum(pkt, 8);
    return pkt;
}

void rel_resend_pkts(rel_t* s){
    // Now in milliseconds
    struct timeval now;
    gettimeofday(&now, NULL);
    long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;

    buffer_node_t* node = buffer_get_first(s->send_buffer);
    while(node != NULL){
        if(node->last_retransmit + s->timeout < now_ms){
            node->packet.ackno = htonl(s->rec_ackno);
            node->packet.cksum = htons(0);
            node->packet.cksum = cksum(&node->packet, ntohs(node->packet.len));
            conn_sendpkt(s->c, &node->packet, ntohs(node->packet.len));
            node->last_retransmit = now_ms;
            return;
            
        }
        node = node->next;
    }
}



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

    /* Do any other initialization you need here */

    r->send_max_window_size = cc->window;
    r->send_next_not_alloc = 1;
    r->send_sliding_window_start = 1;
    r->send_eof = 0;

    r->rec_max_window_size = cc->window;
    r->rec_sliding_window_start = 1;
    r->rec_window_size = 0;
    r->rec_eof = 0;
    r->rec_ackno = 1;

    r->timeout = (long) cc->timeout;

    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // ...
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next)
        r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...
    free(r);
}


void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    // translate from networkt to host
    uint16_t checksum = ntohs(pkt->cksum);
    size_t pkt_size = ntohs(pkt->len);
    size_t ackno = ntohl(pkt->ackno);

    // reset cksum
    pkt->cksum = htons(0);

    // check for corruption
    if(ntohs(cksum(pkt, n)) != checksum || n != pkt_size){
        return;
    }

    // ack packet
    if(n == 8){
        if(buffer_remove(r->send_buffer, ackno) > 0){
            rel_read(r);
        }
        return;
    }

    // data packet
    if(n >= 12 && n <= 512){
        //buffer_remove(r->send_buffer, ackno);
        uint32_t seqno = ntohl(pkt->seqno);
        //fprintf(stderr, "packet arrived : %i\n", seqno);

        // check whether packet outside sliding window;
        if(r->rec_sliding_window_start + r->rec_max_window_size < seqno){
            fprintf(stderr, "packet outside sliding window : %i, %i\n", seqno, r->rec_ackno);
            return;
        }
        if(r->rec_ackno > seqno){
            // send ack
            packet_t* ack_pkt = rel_make_ack_pkt(r->rec_ackno);
            conn_sendpkt(r->c, ack_pkt, 8);
            return;
        }

        // eof packet
        if(n == 12){
            //fprintf(stderr, "eof packet : %i\n", seqno);
            r->rec_eof = 1;
        }

        // insert new packet
        buffer_insert(r->rec_buffer, pkt, 0);

        // update ackno
        buffer_node_t* node = buffer_get_first(r->rec_buffer);
        while(node != NULL){
            if(ntohl(node->packet.seqno) == r->rec_ackno){
                r->rec_ackno = ntohl(node->packet.seqno) + 1;
            }
            node = node->next;
        }

        // send ack
        packet_t* ack_pkt = rel_make_ack_pkt(r->rec_ackno);
        conn_sendpkt(r->c, ack_pkt, 8);

        // try to output the received packet
        rel_output(r);

        return;
    }
}


void
rel_read (rel_t *r)
{
    // return if slidingwindow is at max size or there is already a small packet in the queue
    if(buffer_size(r->send_buffer) >= r->send_max_window_size || r->send_eof == 1){
        return;
    }

    // Now in milliseconds
    struct timeval now;
    gettimeofday(&now, NULL);
    long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;

    char data[500];
    int data_len = conn_input(r->c, data, 500);
    while(data_len > 0 && buffer_size(r->send_buffer) < r->send_max_window_size){

        packet_t* pkt = rel_make_data_pkt(data_len, r->send_next_not_alloc, data, r->rec_ackno);
        buffer_insert(r->send_buffer, pkt, now_ms);
        r->send_next_not_alloc++;
        conn_sendpkt(r->c, pkt, ntohs(pkt->len));
        if(buffer_size(r->send_buffer) >= r->send_max_window_size){
            return;
        }
        data_len = conn_input(r->c, data, 500);
    }
    if(data_len == -1){
        packet_t* pkt = rel_make_eof_pkt(r->send_next_not_alloc, r->rec_ackno);
        buffer_insert(r->send_buffer, pkt, now_ms);
        conn_sendpkt(r->c, pkt, 12);
        r->send_next_not_alloc++;
        r->send_eof = 1;
    }

}

void
rel_output (rel_t *r)
{
    uint16_t space = (uint16_t)conn_bufspace(r->c);
    buffer_node_t* node = buffer_get_first(r->rec_buffer);
    while(node != NULL){
        // check whether there is enough space in the output buffer
        if(space < ntohs(node->packet.len) - 12){
            return;
        }
        // check if all previous packets have arrived
        if(ntohl(node->packet.seqno) >= r->rec_ackno){
            return;
        }
        conn_output(r->c, node->packet.data, ntohs(node->packet.len) - 12);
        space = (uint16_t)conn_bufspace(r->c);
        r->rec_sliding_window_start = ntohl(node->packet.seqno);
        buffer_remove_first(r->rec_buffer);
        node = node->next;
    }
}

void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    rel_t *dest;
    while (current != NULL) {

        rel_read(current);

        // try outputing data
        rel_output(current);

        // resend packets whose timout has been triggered
        rel_resend_pkts(current);

        // check whether the connection can be destroyed
        dest = current;
        current = current->next;
        if(dest->send_eof == 1 && buffer_size(dest->send_buffer) <= 1 ){
            if(dest->rec_eof == 1 && buffer_size(dest->rec_buffer) == 0){
                fprintf(stderr, "connection destroyed\n");
                rel_destroy(dest);
            }
        }
    }

    // if s->send_eof == 1 and buffer_size(s->send_buffer) == 0 -> out eof completed

}
