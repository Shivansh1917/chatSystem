CXX = g++
CXXFLAGS = -std=c++17 -pthread -Wall

TARGETS = chat_server discovery_server client

all: $(TARGETS)

chat_server: chat_server_multithreaded.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

discovery_server: discovery_server.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

client: client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(TARGETS)