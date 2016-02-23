CXXFLAGS := -Wall -g -std=c++11 -I.

HDRS := $(wildcard *.h)
SRCS := $(wildcard *.cpp)
OBJS := $(patsubst %.cpp,%.o,$(SRCS))

TARGET := ledsrv

all: Makefile $(TARGET)

$(TARGET): $(HDRS) $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@

clean:
	rm -rf *.o $(TARGET)

.PHONY: all clean