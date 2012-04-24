// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#ifndef IMPALA_EXEC_DELIMITED_TEXT_PARSER_H
#define IMPALA_EXEC_DELIMITED_TEXT_PARSER_H

#include "exec/hdfs-scanner.h"
#include "exec/hdfs-scan-node.h"

namespace impala {

class DelimitedTextParser {
 public:
  // Intermediate structure used for two pass parsing approach. In the first pass,
  // the FieldLocation structs are filled out and contain where all the fields start and
  // their lengths.  In the second pass, the FieldLocation is used to write out the
  // slots. We want to keep this struct as small as possible.
  struct FieldLocation {
    //start of field
    char* start;
    // Encodes the length and whether or not this fields needs to be unescaped.
    // If len < 0, then the field needs to be unescaped.
    int len;
  };

  // The Delimited Text Parser parses text rows that are delimited by specific
  // characters:
  //   tuple_delim: delimits tuples
  //   field_delim: delimits fields
  //   collection_item_delim: delimits collection items
  //   escape_char: escape delimiters, make them part of the data.
  // Other parameters to the creator:
  //   map_column_to_slot: maps a column in the input to the output slot.
  //   start_column: the index in the above vector where the columns start.
  //                 it will be non-zero if there are partition columns.
  //   timer: timer to use to time the parsing operation, or NULL.
  //
  // The main method is ParseData which fills in a vector of
  // pointers and lengths to the fields.  It also can handle an excape character
  // which masks a tuple or field delimiter that occurs in the data.
  DelimitedTextParser(const std::vector<int>& map_column_to_slot, int start_column,
                      char tuple_delim, char field_delim_ = '\0',
                      char collection_item_delim = '\0', char escape_char = '\0');

  // Called to initialize parser at beginning of scan range.
  void ParserReset();

  // Check if we are at the start of a tuple.
  bool AtTupleStart() { return column_idx_ == start_column_; }

  // Parses a byte buffer for the field and tuple breaks.
  // This function will write the field start & len to field_locations
  // which can then be written out to tuples.
  // This function uses SSE ("Intel x86 instruction set extension
  // 'Streaming Simd Extension') if the hardware supports SSE4.2
  // instructions.  SSE4.2 added string processing instructions that
  // allow for processing 16 characters at a time.  Otherwise, this
  // function walks the file_buffer_ character by character.
  // Input Parameters:
  //   max_tuples: The maximum number of tuples that should be parsed.
  //               This is used to control how the batching works.
  //   remeing_len: Length of data remaining in the byte_buffer_pointer.
  //   byte_buffer_pointer: Pointer to the buffer containing the data to be parsed.
  // Output Parameters:
  //   field_locations: Vector of pointers to data fields and their lengths
  //   num_tuples: Number of tuples parsed
  //   num_fields: Number of materialized fields parsed
  //   next_column_start: pointer within file_buffer_ where the next field starts
  //                      after the return from the call to ParseData
  Status ParseFieldLocations(int max_tuples, int64_t remaining_len,
      char** byte_buffer_ptr, std::vector<FieldLocation>* field_locations,
      int* num_tuples, int* num_fields, char** next_column_start);

  // FindFirstInstance returns the position after the first non-escaped tuple
  // delimiter from the starting offset.
  // Used to find the start of a tuple if jumping into the middle of a text file.
  // Also used to find the sync marker for Sequenced and RC files.
  int FindFirstInstance(char* buffer, int len);

  // Find a sync block if jumping into the middle of a Sequence or RC file.
  // The sync block is always proceeded by an indicator of 4 bytes of -1 (0xff).
  // This will move the Bytestream to the begining of the sync indicator (the -1).
  // If no sync block is found we will be at or beyond the end of the
  // scan range.
  // Inputs:
  //   end_of_range: the end of the scan range we are searching.
  //   sync_size: the size of the sync block.
  //   sync: the sync bytes to look for.
  //   byte_stream: the byte stream to read.
  Status FindSyncBlock(int end_of_range, int sync_size, uint8_t*
                       sync, ByteStream*  byte_stream);

  // Will we return the current column to the query?
  bool ReturnCurrentColumn() {
    return map_column_to_slot_[column_idx_] != HdfsScanNode::SKIP_COLUMN;
  }

 private:
  // Initialize the parser state.
  void ParserInit(HdfsScanNode* scan_node);

  // Helper routine to add a column to the field_locations vector.
  // Input:
  //   len: lenght of the current column.
  // Input/Output:
  //   next_column_start: Start of the current column, moved to the start of the next.
  //   num_fields: current number of fileds processed, updated to next field.
  // Output:
  //   field_locations: updated with start and length of current field.
  void AddColumn(int len, char** next_column_start, int* num_fields,
                 std::vector<FieldLocation>* field_locations);

  // Map columns in the data to slots in the tuples.
  const std::vector<int>& map_column_to_slot_;

  // First non-partition column that will be extracted from parsed data.
  int start_column_;

  // SSE(xmm) register containing the tuple search character.
  __m128i xmm_tuple_search_;

  // SSE(xmm) register containing the delimiter search character.
  __m128i xmm_delim_search_;

  // SSE(xmm) register containing the escape search character.
  __m128i xmm_escape_search_;

  // Character delimiting fields (to become slots).
  char field_delim_;

  // Escape character.
  char escape_char_;

  // Character delimiting collection items (to become slots).
  char collection_item_delim_;

  // Character delimiting tuples.
  char tuple_delim_;

  // Whether or not the current column has an escape character in it
  // (and needs to be unescaped)
  bool current_column_has_escape_;

  // Whether or not the previous character was the escape character
  bool last_char_is_escape_;
  
  // Precomputed masks to process escape characters
  uint16_t low_mask_[16];
  uint16_t high_mask_[16];

  // Index to keep track of the current current column in the current file
  int column_idx_;
};

}// namespace impala
#endif// IMPALA_EXEC_DELIMITED_TEXT_PARSER_H
