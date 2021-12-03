#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "syscall.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255
#define TABLE_MAX_PAGES 100

typedef enum {
  META_COMMAND_SUCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
  PREPARE_SUCESS,
  PREPARE_UNRECONIZED_COMMAND,
  PREPARE_SYNTAX_ERROR,
  PREPARE_STRING_TOO_LONG,
  PREPARE_NEGATIVE_ID
} PrepareResult;

typedef enum {
  EXECUTE_SUCCESS,
  EXECUTE_TABLE_FULL,
  EXECUTE_DUPLICATE_KEY
} ExecutableResult;

typedef enum {
  STATEMENT_INSERT,
  STATEMENT_SELECT
} StatementType;

typedef enum {
  NODE_INTERNAL,
  NODE_LEAF
} NodeType;

typedef struct {
  char *buffer;
  size_t buffer_length;
  size_t input_length;
} InputBuffer;

typedef struct {
  uint32_t id;
  char username[COLUMN_USERNAME_SIZE + 1];
  char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef struct {
  StatementType type;
  Row row_to_insert;
} Statement;

typedef struct {
  int file_descriptor;
  uint32_t file_length;
  uint32_t num_pages;
  void *pages[TABLE_MAX_PAGES];
} PAGER;

typedef struct {
  uint32_t root_page_num;
  PAGER *pager;
} Table;

typedef struct {
  Table *table;
  uint32_t page_num;
  uint32_t cell_num;
  bool end_of_table;
} Cursor;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

const uint32_t PAGE_SIZE = 4096;

/* Node header Layout */

const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
const uint32_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

/* Lead node header Layout */

const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE;

/* Leaf node Body layout */

const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET = 0;
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;
const uint32_t LEAF_NODE_VALUE_OFFSET = LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE;
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;

uint32_t *leaf_node_num_cells(void *node) {
  return node + LEAF_NODE_NUM_CELLS_OFFSET;
}

void *leaf_node_cell(void *node, uint32_t cell_num) {
  return node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

uint32_t *leaf_node_key(void *node, uint32_t cell_num) {
  return leaf_node_cell(node, cell_num);
}

void *leaf_node_value(void *node, uint32_t cell_num) {
  return leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

NodeType get_node_type(void *node) {
  uint8_t value = *((uint8_t *)(node + NODE_TYPE_OFFSET));
  return (NodeType)value;
}

void set_node_type(void *node, NodeType type) {
  uint8_t value = type;
  *((uint8_t *)(node + NODE_TYPE_OFFSET)) = value;
}

void initialize_leaf_node(void *node) {
  set_node_type(node, NODE_LEAF);
  *leaf_node_num_cells(node) = 0; 
}

InputBuffer *new_input_buffer() {
  InputBuffer *input_buffer = (InputBuffer *) malloc(sizeof(InputBuffer));
  input_buffer -> buffer = NULL;
  input_buffer -> buffer_length = 0;
  input_buffer -> input_length = 0;

  return input_buffer;
}

void print_prompt(void) { printf("db >"); }

void read_input(InputBuffer *input_buffer) {
  ssize_t bytes_read = 
    getline(&(input_buffer -> buffer), &(input_buffer -> buffer_length), stdin);
  
  if (bytes_read <= 0){
    printf("Error reading input\n");
    exit(EXIT_FAILURE);
  }

  input_buffer ->input_length = bytes_read - 1;
  input_buffer -> buffer[bytes_read - 1] = 0;
}

void close_input_buffer(InputBuffer *input_buffer) {
  free(input_buffer);
  free(input_buffer -> buffer);
}

void *get_page(PAGER *pager, uint32_t page_num) {
  if (page_num > TABLE_MAX_PAGES) {
    printf("Tried to fecth page number out of bounds %d\n", TABLE_MAX_PAGES);
    exit(EXIT_FAILURE);
  }

  if (pager -> pages[page_num] == NULL) {
    void *page = malloc(PAGE_SIZE);
    uint32_t num_pages = pager -> file_length / PAGE_SIZE;

    if (pager -> file_length % PAGE_SIZE){
      num_pages += 1;
    }

    if (page_num <= num_pages) {
      lseek(pager -> file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
      ssize_t bytes_read = read(pager -> file_descriptor, page, PAGE_SIZE);

      if (bytes_read == -1) {
        printf("Error read file: %d\n", errno);
        exit(EXIT_FAILURE);
      }
    }

    pager -> pages[page_num] = page;

    if (page_num >= pager -> num_pages) {
      pager -> num_pages = page_num + 1;
    }
  }

  return pager->pages[page_num];
}

Cursor *leaf_node_find(Table *table, uint32_t page_num, uint32_t key) {
  void *node = get_page(table -> pager, page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  Cursor *cursor = malloc(sizeof(Cursor));
  cursor -> table = table;
  cursor -> page_num = page_num;

  uint32_t min_index = 0;
  uint32_t one_past_max_index = num_cells;

  while (one_past_max_index != min_index) {
    uint32_t index =  (min_index + one_past_max_index) / 2;
    uint32_t key_at_index = *leaf_node_key(node, index);
    if (key == key_at_index) {
      cursor -> cell_num = index;
      return cursor;
    }
    if (key < key_at_index) {
      one_past_max_index = index;
    } else {
      min_index = index + 1;
    }
  }

  cursor -> cell_num = min_index;
  return cursor;
}

PAGER *page_open(const char *filename) {
  int fd = open(filename,
    O_RDWR | O_CREAT, S_IWUSR | S_IRUSR
  );

  if (fd == -1) {
    printf("Unable to open file\n");
    exit(EXIT_FAILURE);
  }

  off_t file_length = lseek(fd, 0, SEEK_END);

  PAGER* pager = malloc(sizeof(PAGER));
  pager -> file_descriptor = fd;
  pager -> file_length = file_length;
  pager -> num_pages = (file_length / PAGE_SIZE);

  if (file_length % PAGE_SIZE != 0) {
    printf("Db file is not a whole number of pages. Corrupt file\n");
    exit(EXIT_FAILURE);
  }

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    pager -> pages[i] = NULL;
  }

  return pager;
}

Table *db_open(const char *filename) {
  PAGER *pager = page_open(filename);

  Table *table = malloc(sizeof(table));
  table -> pager = pager;
  table -> root_page_num = 0;

  if (pager -> num_pages == 0) {
    void *root_node = get_page(pager, 0);
    initialize_leaf_node(root_node);
  }

  return table;
}

void pager_flush(PAGER *pager, uint32_t page_num) {
  if (pager -> pages[page_num] == NULL) {
    printf("Trie to flush null page\n");
    exit(EXIT_FAILURE);
  }

  off_t offset = lseek(pager -> file_descriptor, page_num * PAGE_SIZE, SEEK_SET);

  if (offset == -1) {
    printf("Error seeking: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  ssize_t bytes_written = write(pager -> file_descriptor, pager -> pages[page_num], PAGE_SIZE);

  if (bytes_written == -1) {
    printf("Error wirting: %d\n", errno);
    exit(EXIT_FAILURE);
  }
}

void db_close(Table *table) {
  PAGER* pager = table->pager;

  for (uint32_t i = 0; i < pager -> num_pages; i++) {
     if (pager->pages[i] == NULL) {
        continue;
     }
     
    pager_flush(pager, i);
    free(pager->pages[i]);
    pager->pages[i] = NULL;
  }

  int result = close(pager->file_descriptor);
  if (result == -1) {
     printf("Error closing db file.\n");
     exit(EXIT_FAILURE);
  }
  
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
     void* page = pager->pages[i];
     if (page) {
         free(page);
         pager->pages[i] = NULL;
     }
  }

  free(pager);
  free(table);
}

Cursor *table_start(Table *table) {
  Cursor *cursor = malloc(sizeof(Cursor));
  cursor -> table = table;
  cursor -> page_num = table -> root_page_num;
  cursor -> cell_num = 0;

  void *root_node = get_page(table -> pager, table -> root_page_num);
  uint32_t num_cells = *leaf_node_num_cells(root_node);
  cursor -> cell_num = num_cells;

  return cursor;
}


void cursor_advance(Cursor *cursor) {
  uint32_t page_num = cursor -> page_num;
  void *node = get_page(cursor -> table -> pager, page_num);

  cursor -> cell_num += 1;
  if (cursor -> cell_num >= (*leaf_node_num_cells(node)))
    cursor -> end_of_table = true;
}


PrepareResult prepare_insert(InputBuffer *input_buffer, Statement *statemet) {
  statemet -> type = STATEMENT_INSERT;

  char *keyword = strtok(input_buffer -> buffer, " ");
  char *id_string = strtok(NULL, " ");
  char *username = strtok(NULL, " ");
  char *email = strtok(NULL, " ");

  if (id_string == NULL || username == NULL || email == NULL) 
    return PREPARE_SYNTAX_ERROR;

  int id = atoi(id_string);

  if (id < 0) return PREPARE_NEGATIVE_ID;

  if (strlen(username) > COLUMN_USERNAME_SIZE)
    return PREPARE_STRING_TOO_LONG;
  
  if (strlen(email) > COLUMN_EMAIL_SIZE)
    return PREPARE_STRING_TOO_LONG;

  statemet -> row_to_insert.id = id;
  strcpy(statemet -> row_to_insert.username, username);
  strcpy(statemet -> row_to_insert.email, email);

  return PREPARE_SUCESS;
}

PrepareResult prepare_statement(InputBuffer *input_buffer, Statement *statement) {
  if (strncmp(input_buffer -> buffer, "insert", 6) == 0) 
    return prepare_insert(input_buffer, statement);
  

  if (strncmp(input_buffer -> buffer, "select", 6) == 0) {
    statement -> type = STATEMENT_SELECT;
    return PREPARE_SUCESS;
  }

  return PREPARE_UNRECONIZED_COMMAND;
}

void serialize_row(Row *source, void *destination) {
  memcpy(destination + ID_OFFSET, &(source -> id), ID_SIZE);
  strncpy(destination + USERNAME_OFFSET, source -> username, USERNAME_SIZE);
  strncpy(destination + EMAIL_OFFSET, source -> email, EMAIL_SIZE);
}

void deserialize_row(void *source, Row *destination) {
  memcpy(&(destination -> id), source + ID_OFFSET, ID_SIZE);
  memcpy(&(destination -> username), source + USERNAME_OFFSET, USERNAME_SIZE);
  memcpy(&(destination -> email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

void *cursor_value(Cursor *cursor) {
  uint32_t page_num = cursor -> page_num;

  void *page = get_page(cursor -> table -> pager, page_num);

  return leaf_node_value(page, cursor -> cell_num);
}

void print_row(Row* row) {
  printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

Cursor *table_find(Table *table, uint32_t key) {
  uint32_t root_page_num = table -> root_page_num;
  void *root_node = get_page(table -> pager, root_page_num);

  if (get_node_type(root_node) == NODE_LEAF) {
    return leaf_node_find(table, root_page_num, key);
  } else {
    printf("Need to implement seacrhing an internal node\n");
    exit(EXIT_FAILURE);
  }
}

void leaf_node_insert(Cursor *cursor, uint32_t key, Row *value) {
  void *node = get_page(cursor -> table -> pager, cursor -> page_num);

  uint32_t num_cells = *leaf_node_num_cells(node);
  if (num_cells >= LEAF_NODE_MAX_CELLS){
    printf("Need to implement splitting a leaft node\n");
    exit(EXIT_FAILURE);
  }

  if (cursor -> cell_num < num_cells) {
    for (uint32_t i = num_cells; i > cursor -> cell_num; i--){
      memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1), 
      LEAF_NODE_CELL_SIZE);
    }
  }

  *(leaf_node_num_cells(node)) += 1;
  *(leaf_node_key(node, cursor -> cell_num)) = key;
  
  serialize_row(value, leaf_node_value(node, cursor -> cell_num));  
}

void print_leaf_node(void *node) {
  uint32_t num_cells = *leaf_node_num_cells(node);
  printf("leaft (size %d)\n", num_cells);
  
  for (uint32_t i = 0; i < num_cells; i++) {
    uint32_t key = *leaf_node_key(node, i);
    printf(" - %d : %d\n", i, key);
  }
}

ExecutableResult execute_insert(Statement *statement, Table *table) {
  void *node = get_page(table -> pager, table -> root_page_num);

  uint32_t num_cells = (*leaf_node_num_cells(node));
  if ( num_cells >= LEAF_NODE_MAX_CELLS) 
    return EXECUTE_TABLE_FULL;
  
  Row *row_to_insert = &(statement->row_to_insert);
  uint32_t key_to_insert = row_to_insert -> id;
  Cursor *cursor = table_find(table, key_to_insert);

  if (cursor -> cell_num < num_cells) {
    uint32_t key_at_index = *leaf_node_key(node, cursor -> cell_num);
    if (key_at_index == key_to_insert) {
      return EXECUTE_DUPLICATE_KEY;
    }
  }

  leaf_node_insert(cursor, key_to_insert, row_to_insert);

  free(cursor);

  return EXECUTE_SUCCESS;
}

ExecutableResult execute_select(Statement *statement, Table *table) {
  Cursor *cursor = table_start(table);
  Row row;

  while (!(cursor -> end_of_table)) {
    deserialize_row(cursor_value(cursor), &row);
    print_row(&row);
    cursor_advance(cursor);
  }

  free(cursor);

  return EXECUTE_SUCCESS;
}

ExecutableResult execute_statements(Statement *statement, Table *table) {
  switch (statement -> type){
    case STATEMENT_INSERT:
      return execute_insert(statement, table);
    case STATEMENT_SELECT:
      return execute_select(statement, table);
  }
}

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table) {
  if (strcmp(input_buffer -> buffer, ".exit") == 0) {
    db_close(table);
    exit(EXIT_SUCCESS);
  }else if (strcmp(input_buffer -> buffer, ".btree") == 0) {
    print_leaf_node(get_page(table -> pager, 0));
    return META_COMMAND_SUCESS;
  } 
  else {
    return META_COMMAND_UNRECOGNIZED_COMMAND;
  }
}

int main (int argc, char *argv[]) {
  InputBuffer *input_buffer = new_input_buffer();
  
  if (argc < 2) {
    printf("Must supply a database filename\n");
    exit(EXIT_FAILURE);
  }

  char *filename = argv[1];
  Table *table = db_open(filename);

  while (true){
    print_prompt();
    read_input(input_buffer);
    
    if (input_buffer -> buffer[0] == '.') {
      switch(do_meta_command(input_buffer, table)) {
        case (META_COMMAND_SUCESS):
          continue;
        case (META_COMMAND_UNRECOGNIZED_COMMAND):
          printf("Unregonized command %s \n", input_buffer -> buffer);
          continue;
    }
  } 
    
    Statement statement;
    switch(prepare_statement(input_buffer, &statement)) {
      case (PREPARE_SUCESS):
        break;
      case (PREPARE_SYNTAX_ERROR):
        printf("Syntax error. Could not parse statement. \n");
        continue;
      case (PREPARE_STRING_TOO_LONG):
        printf("String is too long\n");
        continue;
      case (PREPARE_NEGATIVE_ID):
        printf("ID must be postive\n");
        continue;
      case (PREPARE_UNRECONIZED_COMMAND):
        printf("Unrecognized keyword of '%s' \n", input_buffer -> buffer);
        continue;
    }

    switch(execute_statements(&statement, table)) {
      case (EXECUTE_SUCCESS):
        printf("Executed.\n");
        break;
      case (EXECUTE_DUPLICATE_KEY):
        printf("Error: Duplicated key.\n");
        break;
      case (EXECUTE_TABLE_FULL):
        printf("Error: Table full.\n");
        break;
    }

  }
}