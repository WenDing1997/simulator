#ifndef FLOW_H
#define FLOW_H

#include <unordered_map>
#include "node.h"

class Packet;
class Ack;
class Probe;
class RetxTimeoutEvent;
class FlowProcessingEvent;

class Flow {
    public:
        Flow(uint32_t id, double start_time, uint32_t size, Host *s, Host *d);

        ~Flow(); // Destructor

        virtual void start_flow();
        virtual void send_pending_data();
        virtual Packet *send(uint32_t seq);
        virtual void send_ack(uint32_t seq, std::vector<uint32_t> sack_list);
        virtual void receive_ack(uint32_t ack, std::vector<uint32_t> sack_list);
        void receive_data_pkt(Packet* p);
        virtual void receive(Packet *p);
        
        // Only sets the timeout if needed; i.e., flow hasn't finished
        virtual void set_timeout(double time);
        virtual void handle_timeout();
        virtual void cancel_retx_event();

        virtual uint32_t get_priority(uint32_t seq);
        virtual void increase_cwnd();
        virtual double get_avg_queuing_delay_in_us();

        uint32_t id;
        double start_time;
        double finish_time;
        uint32_t size;
        Host *src;
        Host *dst;
        uint32_t cwnd_mss;
        uint32_t max_cwnd;
        double retx_timeout;
        uint32_t mss;
        uint32_t hdr_size;

        // Sender variables
        uint32_t next_seq_no;
        uint32_t last_unacked_seq;
        RetxTimeoutEvent *retx_event;
        FlowProcessingEvent *flow_proc_event;

        //  std::unordered_map<uint32_t, Packet *> packets;

        // Receiver variables
        std::unordered_map<uint32_t, bool> received;
        uint32_t received_bytes;
        uint32_t recv_till;
        uint32_t max_seq_no_recv;

        uint32_t total_pkt_sent;
        int size_in_pkt;
        int pkt_drop;
        int data_pkt_drop;
        int ack_pkt_drop;
        int first_hop_departure;
        int last_hop_departure;
        uint32_t received_count;
        // Sack
        uint32_t scoreboard_sack_bytes;
        // finished variables
        bool finished;
        double flow_completion_time;
        double total_queuing_time;
        double first_byte_send_time;
        double first_byte_receive_time;

        uint32_t flow_priority;
        double deadline;


        // New fields and functions for logging purposes
        // To measure avg_cwnd: total_cwnd_mss/cwnd_mss_count, and end_cwnd
        void compute_avg_cwnd(uint32_t cwnd_mss); // Did I call compute_avg_cwnd in the right place in flow.cpp? I called in send().
        int cwnd_mss_count;
        uint32_t total_cwnd_mss;
        uint32_t avg_cwnd;
        uint32_t end_cwnd;
        // To measure avg_rtt, max_rtt, end_rtt
        // RTT: pkt_delivery_time_fwd_path + pkt_delivery_time_reverse_path
        virtual void send_ack(uint32_t seq, std::vector<uint32_t> sack_list, double delivery_time_fwd_path);
        double total_rtt; // I think I need ack > last_unacked_seq condition in receive()?
        int rtt_count;
        double avg_rtt;
        double max_rtt;
        double end_rtt;
        // To measure nactv_flows
        uint32_t nactv_flows_when_finished;
        // To mmeasure last_byte_send_time, last_byte_rcvd_time
        // Assuming we're only talking about data pkt, not ack pkt
        double last_byte_send_time;
        double last_byte_rcvd_time;

        // Remaining questions:
        // 1 Time between when the dest receives the packet and when the dest sends ack?
        // 2 Will adding code affect other calculations involving time?
        
};

#endif
