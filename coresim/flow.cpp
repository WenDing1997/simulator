#include <math.h>
#include <iostream>
#include <assert.h>

#include "flow.h"
#include "packet.h"
#include "event.h"

#include "../run/params.h"

extern double get_current_time(); 
extern void add_to_event_queue(Event *);
extern int get_event_queue_size();
extern DCExpParams params;
extern uint32_t num_outstanding_packets;
extern uint32_t max_outstanding_packets;
extern uint32_t duplicated_packets_received;

// For logging purposes
extern uint32_t nactv_flows;

Flow::Flow(uint32_t id, double start_time, uint32_t size, Host *s, Host *d) {
    this->id = id;
    this->start_time = start_time;
    this->finish_time = 0;
    this->size = size;
    this->src = s;
    this->dst = d;

    this->next_seq_no = 0;
    this->last_unacked_seq = 0;
    this->retx_event = NULL;
    this->flow_proc_event = NULL;

    this->received_bytes = 0;
    this->recv_till = 0;
    this->max_seq_no_recv = 0;
    this->cwnd_mss = params.initial_cwnd;
    this->max_cwnd = params.max_cwnd;
    this->finished = false;

    //SACK
    this->scoreboard_sack_bytes = 0;

    this->retx_timeout = params.retx_timeout_value;
    this->mss = params.mss;
    this->hdr_size = params.hdr_size;
    this->total_pkt_sent = 0;
    this->size_in_pkt = (int)ceil((double)size/mss);

    this->pkt_drop = 0;
    this->data_pkt_drop = 0;
    this->ack_pkt_drop = 0;
    this->flow_priority = 0;
    this->first_byte_send_time = -1;
    this->first_byte_receive_time = -1;
    this->first_hop_departure = 0;
    this->last_hop_departure = 0;

    // New fields for logging purposes
    this->total_cwnd_mss= 0;
    this->cwnd_mss_count = 0;
    this->avg_cwnd = 0;
    this->end_cwnd = 0;
    this->size_in_pkts = size / mss;
    this->total_rtt = 0;
    this->rtt_count = 0;
    this->avg_rtt = 0;
    this->max_rtt = 0;
    this->end_rtt = 0;
    this->max_cwnd_during_flow = 0;
    this->nactv_flows_when_finished = 0;
    this->last_byte_send_time = 0;
    this->last_byte_rcvd_time = 0;
    this->init_seq = 0;
    this->end_seq = 0;
    this->bytes_sent = 0;
    this->nack_pkts = 0;
    this->nack_bytes = 0;
    this->tot_cwnd_cuts = 0;
    this->nrexmit = 0;
    this->ndup_acks = 0;
}

Flow::~Flow() {
    //  packets.clear();
}

void Flow::start_flow() {
    nactv_flows += 1;
    send_pending_data();
}

void Flow::compute_avg_cwnd(uint32_t cwnd_mss) {
    cwnd_mss_count++;
    total_cwnd_mss += cwnd_mss;
    end_cwnd = cwnd_mss < size_in_pkts ? cwnd_mss : size_in_pkts;
    if (end_cwnd > max_cwnd_during_flow) {
        max_cwnd_during_flow = end_cwnd;
    }
    avg_cwnd = total_cwnd_mss/cwnd_mss_count;
    avg_cwnd = avg_cwnd < size_in_pkts ? avg_cwnd : size_in_pkts;
}

void Flow::send_pending_data() {
    if (received_bytes < size) {
        uint32_t seq = next_seq_no;
        uint32_t window = cwnd_mss * mss + scoreboard_sack_bytes;
        //compute_avg_cwnd(cwnd_mss);
        while (
            (seq + mss <= last_unacked_seq + window) &&
            ((seq + mss <= size) || (seq != size && (size - seq < mss)))
        ) {
            // TODO Make it explicit through the SACK list
            if (received.count(seq) == 0) {
                send(seq);
            }

            if (seq + mss < size) {
                next_seq_no = seq + mss;
                seq += mss;
            } else {
                next_seq_no = size;
                seq = size;
            }

            if (retx_event == NULL) {
                set_timeout(get_current_time() + retx_timeout);
            }
        }
    }
}

Packet *Flow::send(uint32_t seq) {
    Packet *p = NULL;

    uint32_t pkt_size;
    if (seq + mss > this->size) {
        pkt_size = this->size - seq + hdr_size;
    } else {
        pkt_size = mss + hdr_size;
    }

    uint32_t priority = get_priority(seq);
    p = new Packet(
            get_current_time(), 
            this, 
            seq, 
            priority, 
            pkt_size, 
            src, 
            dst
            );
    this->total_pkt_sent++;
    if (init_seq == 0) {
        init_seq = seq;
    }
    end_seq = seq;
    bytes_sent += pkt_size;

    add_to_event_queue(new PacketQueuingEvent(get_current_time(), p, src->queue));
    compute_avg_cwnd(cwnd_mss);
    if (p->sending_time > last_byte_send_time) {
        last_byte_send_time = p->sending_time;
    }
    return p;
}

void Flow::send_ack(uint32_t seq, std::vector<uint32_t> sack_list) {
    Packet *p = new Ack(this, seq, sack_list, hdr_size, dst, src); //Acks are dst->src
    add_to_event_queue(new PacketQueuingEvent(get_current_time(), p, dst->queue));
}

// Overload send_ack for logging purposes
void Flow::send_ack(uint32_t seq, std::vector<uint32_t> sack_list, Packet* p) {
    Packet *a = new Ack(this, seq, sack_list, hdr_size, dst, src); //Acks are dst->src
    a->delivery_time_fwd_path = p->delivery_time_fwd_path;
    a->fwd_path_start_time = p->sending_time;
    add_to_event_queue(new PacketQueuingEvent(get_current_time(), a, dst->queue));
}

void Flow::receive_ack(uint32_t ack, std::vector<uint32_t> sack_list) {
    this->scoreboard_sack_bytes = sack_list.size() * mss;

    // On timeouts; next_seq_no is updated to the last_unacked_seq;
    // In such cases, the ack can be greater than next_seq_no; update it
    if (next_seq_no < ack) {
        next_seq_no = ack;
    }

    // New ack!
    if (ack > last_unacked_seq) {
        // Update the last unacked seq
        last_unacked_seq = ack;

        // Adjust cwnd
        increase_cwnd();

        // Send the remaining data
        send_pending_data();

        // Update the retx timer
        if (retx_event != NULL) { // Try to move
            cancel_retx_event();
            if (last_unacked_seq < size) {
                // Move the timeout to last_unacked_seq
                double timeout = get_current_time() + retx_timeout;
                set_timeout(timeout);
            }
        }

    }

    if (ack == size && !finished) {
        nactv_flows--;
        nactv_flows_when_finished = nactv_flows;
        finished = true;
        received.clear();
        finish_time = get_current_time();
        flow_completion_time = finish_time - start_time;
        FlowFinishedEvent *ev = new FlowFinishedEvent(get_current_time(), this);
        add_to_event_queue(ev);
    }
}


void Flow::receive(Packet *p) {
    if (finished) {
        delete p;
        return;
    }

    double recieved_time = get_current_time();
    if (p->type == ACK_PACKET) {
        Ack *a = (Ack *) p;

        // Compute RTT
        if (a->seq_no > last_unacked_seq) { // Why this condition??
            a->delivery_time_reverse_path = recieved_time - a->sending_time;
            // this->end_rtt = a->delivery_time_fwd_path + a->delivery_time_reverse_path; // doesn't include time reciever spent processing pkt, i.e. time between pkt is recieved and ack is sent
            this->end_rtt = recieved_time - a->fwd_path_start_time; // includes time mentioned in comments above
            if (end_rtt > max_rtt) {
                max_rtt = end_rtt;
            }
            total_rtt += end_rtt;
            rtt_count += 1;
            avg_rtt = total_rtt / rtt_count;
        }

        receive_ack(a->seq_no, a->sack_list);
    }
    else if(p->type == NORMAL_PACKET) {
        if (this->first_byte_receive_time == -1) {
            this->first_byte_receive_time = get_current_time();
        }
        p->delivery_time_fwd_path = recieved_time - p->sending_time;
        if (recieved_time > last_byte_rcvd_time) {
            last_byte_rcvd_time = recieved_time;
        }
        this->receive_data_pkt(p);
    }
    else {
        assert(false);
    }

    delete p;
}

void Flow::receive_data_pkt(Packet* p) {
    received_count++;
    total_queuing_time += p->total_queuing_delay;

    if (received.count(p->seq_no) == 0) {
        received[p->seq_no] = true;
        if(num_outstanding_packets >= ((p->size - hdr_size) / (mss)))
            num_outstanding_packets -= ((p->size - hdr_size) / (mss));
        else
            num_outstanding_packets = 0;
        received_bytes += (p->size - hdr_size);
    } else {
        duplicated_packets_received += 1;
        ndup_acks += 1;
    }
    if (p->seq_no > max_seq_no_recv) {
        max_seq_no_recv = p->seq_no;
    }
    // Determing which ack to send
    uint32_t s = recv_till;
    bool in_sequence = true;
    std::vector<uint32_t> sack_list;
    while (s <= max_seq_no_recv) {
        if (received.count(s) > 0) {
            if (in_sequence) {
                if (recv_till + mss > this->size) {
                    recv_till = this->size;
                } else {
                    recv_till += mss;
                }
            } else {
                sack_list.push_back(s);
            }
        } else {
            in_sequence = false;
        }
        s += mss;
    }

    // send_ack(recv_till, sack_list); // Cumulative Ack
    nack_pkts++;
    nack_bytes += p->size;
    send_ack(recv_till, sack_list, p);
}

void Flow::set_timeout(double time) {
    if (last_unacked_seq < size) {
        RetxTimeoutEvent *ev = new RetxTimeoutEvent(time, this);
        add_to_event_queue(ev);
        retx_event = ev;
    }
}


void Flow::handle_timeout() {
    nrexmit++;
    next_seq_no = last_unacked_seq;
    //Reset congestion window to 1
    cwnd_mss = 1;
    send_pending_data(); //TODO Send again
    set_timeout(get_current_time() + retx_timeout);  // TODO
}


void Flow::cancel_retx_event() {
    if (retx_event) {
        retx_event->cancelled = true;
    }
    retx_event = NULL;
}


uint32_t Flow::get_priority(uint32_t seq) {
    if (params.flow_type == 1) {
        return 1;
    }
    if(params.deadline && params.schedule_by_deadline)
    {
        return (int)(this->deadline * 1000000);
    }
    else{
        return (size - last_unacked_seq - scoreboard_sack_bytes);
    }
}


void Flow::increase_cwnd() {
    cwnd_mss += 1;
    if (cwnd_mss > max_cwnd) {
        cwnd_mss = max_cwnd;
    }
}

double Flow::get_avg_queuing_delay_in_us()
{
    return total_queuing_time/received_count * 1000000;
}

