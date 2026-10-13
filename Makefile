CXX = g++
CXXFLAGS = -std=c++17 -pthread -Wall -I/opt/homebrew/opt/nlohmann-json/include

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