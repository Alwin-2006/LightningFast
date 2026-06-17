CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
OBJS     = common.o resp.o store.o protocol.o connection.o main.o
TARGET   = my_redis_server

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
