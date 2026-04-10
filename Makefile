CXX      := clang++
CXXFLAGS := -std=c++23 -O2 -Wall -Wextra
TARGET   := hex

SRC := main.cpp

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

.PHONY: clean
