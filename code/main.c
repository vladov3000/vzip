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
typedef unsigned short UShort;
typedef unsigned char  UChar;
typedef int            Fd;
typedef size_t         Size;
typedef ssize_t        ISize;

typedef struct Tree Tree;

struct Tree {
  // If children[0] == 0, then this is a leaf node and children[1] is a character.
  UShort children[2];
};

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

static UChar buffer[8 * 4096];
static Size  weights[512];
static Tree  trees[512];
static Size  ntrees;
static Queue queues[2];
static Size  encodings[256];
static Size  encoding_lengths[256];
static UChar output[8 * 4096];
static Size  output_byte;
static UChar small_output;
static Size  output_bit;
static Size  input_start;
static ISize input_bytes;
static UChar small_input;
static Size  input_bit;

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

static unsigned read_byte_or_eof(int fd) {
  if (input_start == input_bytes) {
    input_start = 0;
    input_bytes = read(fd, buffer, sizeof buffer);
    if (input_bytes == -1) {
      perror("read");
      exit(EXIT_FAILURE);
    } else if (input_bytes == 0) {
      return 0x1FF;
    }
  }
  return buffer[input_start++];
}

static unsigned peek_byte(int fd) {
  unsigned byte = read_byte_or_eof(fd);
  if (byte != 0x1FF) {
    input_start--;
  }
  return byte;
}

static unsigned read_byte(int fd) {
  unsigned byte = read_byte_or_eof(fd);
  if (byte == 0x1FF) {
    puts("Unexpected EOF.\n");
    exit(EXIT_FAILURE);
  } else {
    return byte;
  }
}

static unsigned read_bit(int fd) {
  if (input_bit == 0) {
    small_input = read_byte(fd);
  }
  unsigned result = small_input & (1 << (7 - input_bit)) ? 1 : 0;
  input_bit       = (input_bit + 1) & 7;
  return result;
}

static void compute_bytes_encodings(Tree* tree, unsigned encoding, int length) {
  UShort* children = tree->children;
  if (children[0] == 0) {
    UChar c             = children[1];
    encodings[c]        = encoding;
    encoding_lengths[c] = length;
    return;
  }

  for (Size i = 0; i < length(tree->children); i++) {
    assert(children[i] != 0);
    Tree* child        = &trees[children[i]];
    Size  new_encoding = (encoding << 1) | i;
    compute_bytes_encodings(child, new_encoding, length + 1);
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
      memset(trees, 0, sizeof trees);

      for (Size i = 0; i < length(queues); i++) {
	Queue* queue = &queues[i];
	queue->start = 0;
	queue->size  = 0;
      }

      {
	Queue* queue = &queues[0];
	for (Size i = 0; i < length(queue->trees); i++) {
	  trees[i].children[0] = 0;
	  trees[i].children[1] = i;
	  queue->trees[i]      = i;
	}
      
	queue->size = length(queue->trees);
	ntrees      = length(queue->trees);
      
	for (Size i = 0; i < sizeof buffer; i++) {
	  weights[queue->trees[buffer[i]]]++;
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

      while (queues[0].size + queues[1].size > 1) {
	UShort children[2];
	for (Size i = 0; i < length(children); i++) {
	  if (queues[0].size == 0) {
	    children[i] = dequeue(&queues[1]);
	  } else if (queues[1].size == 0) {
	    children[i] = dequeue(&queues[0]);
	  } else if (weights[front(&queues[0])] <= weights[front(&queues[1])]) {
	    children[i] = dequeue(&queues[0]);
	  } else {
	    children[i] = dequeue(&queues[1]);
	  }
	}

	assert(ntrees < length(trees));

	Tree* new = &trees[ntrees++];
	for (Size i = 0; i < length(new->children); i++) {
	  weights[new - trees] += weights[children[i]];
	  new->children[i]      = children[i];
	}
	enqueue(&queues[1], new - trees);
      }

      Tree* tree = &trees[front(queues[0].size > 0 ? &queues[0] : &queues[1])];
      for (Size i = 0; i < ntrees; i++) {
	Tree* current = &trees[i];
	if (current->children[0] == 0) {
	  write_byte(output_fd, 0x80);
	  write_byte(output_fd, current->children[1]);
	  write_byte(output_fd, 0);
	  write_byte(output_fd, 0);
	  continue;
	}

	for (Size j = 0; j < length(current->children); j++) {
	  Tree* child = &trees[current->children[j]];
	  assert(child != NULL);
	  
	  short delta = (short) (child - trees);
	  write_byte(output_fd, delta >> 8);
	  write_byte(output_fd, delta & 0xFF);
	}
      }

      compute_bytes_encodings(tree, 0, 0);

      for (Size i = 0; i < bytes_read; i++) {
	unsigned c      = buffer[i] & 0xFF;
	Size   length = encoding_lengths[c];
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
    for (;;) {
      if (peek_byte(input_fd) == 0x1FF) {
	break;
      }
      
      ntrees = 0;
      for (Size i = 0; i < 511; i++) {
	Tree* new = &trees[ntrees++];
	if (peek_byte(input_fd) == 0x80) {
	  read_byte(input_fd);
	  new->children[0] = 0;
	  new->children[1] = read_byte(input_fd);
	  read_byte(input_fd);
	  read_byte(input_fd);
	} else {
	  for (Size j = 0; j < length(new->children); j++) {
	    unsigned head   = read_byte(input_fd) << 8;
	    unsigned tail   = read_byte(input_fd);
	    unsigned offset = head | tail;
	    if (offset >= length(trees)) {
	      printf("Invalid child offset.\n");
	      exit(EXIT_FAILURE);
	    } else {
	      new->children[j] = offset;
	    }
	  }
	}
      }

      assert(ntrees == 511);
      Tree* root = &trees[510];

      for (Size i = 0; i < length(buffer); i++) {
	if (peek_byte(input_fd) == 0x1FF) {
	  goto DONE;
	}
	
	Tree* current = root;
	while (current->children[0] != 0) {
	  unsigned bit = read_bit(input_fd);
	  current      = &trees[current->children[bit]];
	}
	write_byte(output_fd, current->children[1]);
      }

      input_bit = 0;
    }

  DONE:
    if (output_byte > 0) {
      write(output_fd, output, output_byte);
    }
  } else {
    printf("Unknown command \"%s\". Expected compress or decompress.\n", command);
    exit(EXIT_FAILURE);
  }
}
