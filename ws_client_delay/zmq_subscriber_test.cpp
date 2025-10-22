// Simple ZeroMQ subscriber to test receiving MSK messages
// Compile: g++ -o zmq_sub zmq_subscriber_test.cpp -lzmq -std=c++11
// Run: ./zmq_sub

#include <zmq.hpp>
#include <iostream>
#include <string>

int main() {
    std::cout << "ZeroMQ Subscriber - listening for MSK messages..." << std::endl;
    
    try {
        zmq::context_t context(1);
        zmq::socket_t subscriber(context, zmq::socket_type::sub);
        
        // Connect to the publisher
        subscriber.connect("tcp://localhost:5555");
        
        // Subscribe to all messages (empty filter = all topics)
        subscriber.set(zmq::sockopt::subscribe, "");
        
        std::cout << "Connected to tcp://localhost:5555" << std::endl;
        std::cout << "Waiting for messages (Ctrl+C to exit)..." << std::endl;
        std::cout << std::endl;
        
        int msg_count = 0;
        while (true) {
            // Receive topic (first frame)
            zmq::message_t topic_msg;
            auto result = subscriber.recv(topic_msg, zmq::recv_flags::none);
            if (!result) continue;
            
            std::string topic(static_cast<char*>(topic_msg.data()), topic_msg.size());
            
            // Receive message body (second frame)
            zmq::message_t body_msg;
            result = subscriber.recv(body_msg, zmq::recv_flags::none);
            if (!result) continue;
            
            std::string body(static_cast<char*>(body_msg.data()), body_msg.size());
            
            // Display message
            msg_count++;
            std::cout << "[" << msg_count << "] Topic: " << topic << std::endl;
            std::cout << "    Body:  " << body << std::endl;
            std::cout << std::endl;
        }
        
    } catch (const zmq::error_t& e) {
        std::cerr << "ZMQ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
