#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define length(array) (sizeof(array) / sizeof((array)[0]))
#define min(x, y)     ((x) < (y) ? (x) : (y))
#define max(x, y)     ((x) > (y) ? (x) : (y))

typedef char           Char;
typedef ssize_t        ISize;
typedef unsigned char  UChar;
typedef unsigned short UShort;
typedef size_t         Size;
typedef int            Fd;

typedef UShort Tree;

typedef struct {
  UShort trees[256];
  Size   start;
  Size   size;
} Queue;

static UShort front(Queue* queue) {
  assert(queue->size > 0);
  return queue->trees[queue->start % length(queue->trees)];
}

static void enqueue(Queue* queue, UShort tree) {
  assert(queue->size < length(queue->trees));
  UShort* trees = queue->trees;
  trees[(queue->start + queue->size) % length(queue->trees)] = tree;
  queue->size++;
}

static UShort dequeue(Queue* queue) {
  assert(queue->size > 0);
  UShort* trees  = queue->trees;
  UShort  result = trees[queue->start % length(queue->trees)];
  queue->start++;
  queue->size--;
  return result;
}

static Size  weights[512];
static Tree  children[255][2];
static Queue queues[2];
static Size  encodings[256];
static UChar encoding_lengths[256];

static UChar buffer[8 * 4096];

static UChar output[8 * 4096];
static Size  output_byte;
static UChar small_output;
static Size  output_bit;

static void write_byte(int fd, unsigned byte) {
  output[output_byte++] = byte;
  if (output_byte == sizeof output) {    
    write(fd, output, sizeof output);
    output_byte = 0;
  }
}

static void write_bit(int fd, unsigned bit) {
  small_output |= (bit & 1) << (7 - output_bit);
  output_bit++;
  if (output_bit == 8) {
    write_byte(fd, small_output);
    small_output = 0;
    output_bit   = 0;
  }
}

static void compute_bytes_encodings(UShort tree, Size encoding, UChar length) {
  if (tree & 0x100) {
    UChar c             = tree & 0xFF;
    encodings[c]        = encoding;
    encoding_lengths[c] = length;
    return;
  }

  for (Size i = 0; i < 2; i++) {
    Size new_encoding = (encoding << 1) | i;
    compute_bytes_encodings(children[tree][i], new_encoding, length + 1);
  }
}

int main(int argc, Char** argv) {
  if (argc != 4) {
    printf("Expected exactly 3 arguments.\n"
	   "Usage: vzip compress   INPUT OUTPUT\n"
	   "       vzip decompress INPUT OUTPUT\n");
    exit(EXIT_FAILURE);
  }

  Char* command     = argv[1];
  Char* input_path  = argv[2];
  Char* output_path = argv[3];

  Fd input_fd = open(input_path, O_RDONLY);
  if (input_fd == -1) {
    perror("open");
    exit(EXIT_FAILURE);
  }

  Fd output_fd = open(output_path, O_WRONLY | O_CREAT, 0666);
  if (output_fd == -1) {
    perror("open");
    exit(EXIT_FAILURE);
  }

  if (ftruncate(output_fd, 0) == -1) {
    perror("ftruncate");
    exit(EXIT_FAILURE);
  }
  
  if (strcmp(command, "compress") == 0) {
    while (1) {
      ISize bytes_read = read(input_fd, buffer, sizeof buffer);
      if (bytes_read == -1) {
	perror("read");
	exit(EXIT_FAILURE);
      } else if (bytes_read == 0) {
	break;
      }

      memset(weights, 0, sizeof weights);
      memset(children, 0, sizeof children);

      for (Size i = 0; i < length(queues); i++) {
	Queue* queue = &queues[i];
	queue->start = 0;
	queue->size  = 0;
      }

      {
	Queue* queue = &queues[0];
	for (Size i = 0; i < length(queue->trees); i++) {
	  queue->trees[i] = 0x100 | i;
	}
      
	queue->size = length(queue->trees);
      
	for (Size i = 0; i < sizeof buffer; i++) {
	  weights[0x100 | buffer[i]]++;
	}

	for (Size i = 1; i < length(queue->trees); i++) {
	  for (Size j = i; j > 0; j--) {
	    if (weights[queue->trees[j - 1]] > weights[queue->trees[j]]) {
	      UShort swap         = queue->trees[j - 1];
	      queue->trees[j - 1] = queue->trees[j];
	      queue->trees[j]     = swap;
	    } else {
	      break;
	    }
	  }
	}
      }

      Tree current = 0;
      while (queues[0].size + queues[1].size > 1) {
	UShort new_children[2];
	for (Size i = 0; i < 2; i++) {
	  if (queues[0].size == 0) {
	    new_children[i] = dequeue(&queues[1]);
	  } else if (queues[1].size == 0) {
	    new_children[i] = dequeue(&queues[0]);
	  } else if (weights[front(&queues[0])] <= weights[front(&queues[1])]) {
	    new_children[i] = dequeue(&queues[0]);
	  } else {
	    new_children[i] = dequeue(&queues[1]);
	  }
	}

	assert(current < length(children));

	for (Size i = 0; i < 2; i++) {
	  weights[current]     += weights[new_children[i]];
	  children[current][i]  = new_children[i];
	}
	enqueue(&queues[1], current);
	current++;
      }

      assert(current == 255);

      for (Size i = 0; i < current; i++) {
	for (Size j = 0; j < 2; j++) {
	  UShort child = children[i][j];
	  if (child & 0x100) {
	    write_byte(output_fd, 0xFF);
	    write_byte(output_fd, child & 0xFF);
	  } else {
	    write_byte(output_fd, child);
	  }
	}
      }

      UShort root = front(queues[0].size > 0 ? &queues[0] : &queues[1]);
      compute_bytes_encodings(root, 0, 0);

      for (Size i = 0; i < bytes_read; i++) {
	UChar c      = buffer[i];
	Size  length = encoding_lengths[c];
	for (Size j = 0; j < length; j++) {
	  write_bit(output_fd, encodings[c] >> (length - 1 - j));
	}
      }

      if (output_bit > 0) {
	write_byte(output_fd, small_output);
	small_output = 0;
	output_bit   = 0;
      }
    }

    if (output_byte > 0) {
      write(output_fd, output, output_byte);
    }
  } else if (strcmp(command, "decompress") == 0) {
    Size state         = 0;
    Tree current       = 0;
    Size child         = 0;
    Size bytes_written = 0;
    
    while (1) {
      ISize bytes_read = read(input_fd, buffer, sizeof buffer);
      if (bytes_read == -1) {
	perror("read");
	exit(EXIT_FAILURE);
      } else if (bytes_read == 0) {
	if (state < 2) {
	  puts("Unexpected eof while parsing tree.");
	  exit(EXIT_FAILURE);
	} else {
	  break;
	}
      }

      for (Size i = 0; i < bytes_read; i++) {
	UChar byte = buffer[i];
	switch (state) {
	case 0:
	  if (byte == 0xFF) {
	    state = 1;
	  } else {
	    if (byte >= length(children)) {
	      printf("Invalid child offset 0x%x.\n", byte);
	      exit(EXIT_FAILURE);
	    } else {
	      children[current][child] = byte;
	      child++;
	      if (child == 2) {
		child = 0;
		current++;
		if (current == 255) {
		  state         = 2;
		  current       = 254;
		  bytes_written = 0;
		}
	      }
	    }
	  }
	  break;

	case 1:
	  children[current][child] = 0x100 | byte;
	  state = 0;
	  child++;
	  if (child == 2) {
	    child = 0;
	    current++;
	    if (current == 255) {
	      state         = 2;
	      current       = 254;
	      bytes_written = 0;
	    }
	  }
	  break;

	case 2:
	  for (Size j = 0; j < 8; j++) {
	    UChar bit = (byte >> (7 - j)) & 1;
	    current   = children[current][bit];
	    if (current & 0x100) {
	      write_byte(output_fd, current & 0xFF);
	      current = 254;
	      bytes_written++;
	      if (bytes_written == sizeof buffer) {
		state   = 0;
		current = 0;
		child   = 0;
		break;
	      }
	    }
	  }
	  break;
	};
      }
    }
    if (output_byte > 0) {
      write(output_fd, output, output_byte);
    }
  } else {
    printf("Unknown command \"%s\". Expected compress or decompress.\n", command);
    exit(EXIT_FAILURE);
  }
}
