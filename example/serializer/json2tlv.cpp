#include <iostream>
#include <sstream>
#include <fstream>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "ndpi_main.h"

using namespace std;

static pair<char *, size_t> get_corpus(string filename) {
  ifstream is(filename, ios::binary);

  if (is) {
    stringstream buffer;
    buffer << is.rdbuf();
    size_t length = buffer.str().size();
    char * aligned_buffer;
    if (posix_memalign( (void **)&aligned_buffer, 64, (length + 63) / 64  * 64))
      throw "Allocation failed";
    memset(aligned_buffer, 0x20, (length + 63) / 64  * 64);
    memcpy(aligned_buffer, buffer.str().c_str(), length);
    is.close();
    return make_pair((char *)aligned_buffer, length);
  } 

  cerr << "JSON file not found or empty\n";
  exit(1);
}

int key_is_int(char *key) {
  int i, length = strlen(key);

  for (i = 0; i < length; i++)
    if (!isdigit(key[i]))
      return 0;

  return 1;
}

void json_to_tlv(json_object * jobj, ndpi_serializer *serializer) {
  enum json_type type;
  int rc, ikey, ival = 0;
  char *sval = NULL;

  json_object_object_foreach(jobj, key, val) {
    type = json_object_get_type(val);

    //printf("key: %s type: %u ", key, type);

    switch (type) {
      case json_type_int:
        ival = json_object_get_int(val);
      break;
      case json_type_string:
        sval = (char *) json_object_get_string(val);
      break;
      default:
        printf("JSON type %u not supported\n", type);
      break;
    }

    rc = 0;
    if (key_is_int(key)) {
      ikey = atoi(key);
      switch (type) {
        case json_type_int:
          rc = ndpi_serialize_uint32_uint32(serializer, ikey, ival);
        break;
        case json_type_string:
          rc = ndpi_serialize_uint32_string(serializer, ikey, sval);
        break;
        default:
        break;
      }
    } else {
      switch (type) {
        case json_type_int:
          rc = ndpi_serialize_string_uint32(serializer, key, ival);
        break;
        case json_type_string:
          rc = ndpi_serialize_string_string(serializer, key, sval);
        break;
        default:
        break;
      }
    }

    if (rc == -1)
      printf("Serialization error: %d\n", rc);
  }
}

void print_help(char *bin) {
  cerr << "Usage: " << bin << " -v -i <JSON file>\n";
  cerr << "Note: the JSON file should contain an array of records\n";
}

int main(int argc, char *argv[]) {
  char *json_path = NULL;
  int repeat = 1;
  int verbose = 0;
  struct timeval t1, t2;
  uint64_t total_time_usec;
  ndpi_serializer *serializer;
  ndpi_serializer deserializer;
  int i, num_elements;
  int rc;
  char c;

  while ((c = getopt(argc, argv,"hi:v")) != '?') {
    if (c == (char) 255 || c == -1) break;

    switch(c) {
      case 'h':
        print_help(argv[0]);
        exit(0);
      break;
    
      case 'i':
        json_path = strdup(optarg);
      break;

      case 'v':
        verbose = 1;
      break;
    }
  }

  if (json_path == NULL) {
    print_help(argv[0]);
    exit(1);
  }

  /* JSON Import */

  pair<char *, size_t> p = get_corpus(argv[argc - 1]);

  enum json_tokener_error jerr = json_tokener_success;
  char * buffer = (char *) malloc(p.second);
  json_object *f;

  f = json_tokener_parse_verbose(buffer, &jerr);

  if (json_object_get_type(f) == json_type_array)
    num_elements = json_object_array_length(f);
  else
    num_elements = 1;

  printf("%u records found\n", num_elements);

  free(buffer);

  /* nDPI TLV Serialization */

  serializer = (ndpi_serializer *) calloc(num_elements, sizeof(ndpi_serializer)); 

  for (i = 0; i < num_elements; i++) 
    ndpi_init_serializer(&serializer[i], ndpi_serialization_format_tlv);

  total_time_usec = 0;

  for (int r = 0; r < repeat; r++) {

    gettimeofday(&t1, NULL);

    if(json_object_get_type(f) == json_type_array) {
      for(i = 0; i < num_elements; i++) {
        ndpi_reset_serializer(&serializer[i]);
        json_to_tlv(json_object_array_get_idx(f, i), &serializer[i]);
      }
    } else {
      ndpi_reset_serializer(&serializer[0]);
      json_to_tlv(f, &serializer[0]);
    }

    gettimeofday(&t2, NULL);

    total_time_usec += (u_int64_t) ((u_int64_t) t2.tv_sec * 1000000 + t2.tv_usec) - ((u_int64_t) t1.tv_sec * 1000000 + t1.tv_usec);

  }  

  printf("Serialization perf (includes json-c overhead): %.3f msec total time for %u iterations\n", (double) total_time_usec/1000, repeat);

  json_object_put(f);

  /* nDPI TLV Deserialization */

  total_time_usec = 0;

  for (int r = 0; r < repeat; r++) {

    gettimeofday(&t1, NULL);

    for (i = 0; i < num_elements; i++) {
      rc = ndpi_init_deserializer(&deserializer, &serializer[i]);

      if (rc == -1) {
        printf("Deserialization error: %d\n", rc);
        return -1;
      }

      ndpi_serialization_element_type et;
      while ((et = ndpi_deserialize_get_nextitem_type(&deserializer)) != ndpi_serialization_unknown) {
        u_int32_t k32, v32;
        ndpi_string ks, vs;
        u_int8_t bkp, bkpk;

        switch(et) {
          case ndpi_serialization_uint32_uint32:
          ndpi_deserialize_uint32_uint32(&deserializer, &k32, &v32);
          if (verbose) printf("%u=%u ", k32, v32);
          break;

          case ndpi_serialization_uint32_string:
          ndpi_deserialize_uint32_string(&deserializer, &k32, &vs);
          bkp = vs.str[vs.str_len];
          vs.str[vs.str_len] = '\0';
          if (verbose) printf("%u='%s' ", k32, vs.str);
          vs.str[vs.str_len] = bkp;
          break;

          case ndpi_serialization_string_string:
          ndpi_deserialize_string_string(&deserializer, &ks, &vs);
          bkpk = ks.str[ks.str_len], bkp = vs.str[vs.str_len];
          ks.str[ks.str_len] = vs.str[vs.str_len] = '\0';
          if (verbose) printf("%s='%s' ", ks.str, vs.str);
          ks.str[ks.str_len] = bkpk, vs.str[vs.str_len] = bkp;
          break;

          case ndpi_serialization_string_uint32:
          ndpi_deserialize_string_uint32(&deserializer, &ks, &v32);
          bkpk = ks.str[ks.str_len];
          ks.str[ks.str_len] = '\0';
          if (verbose) printf("%s=%u ", ks.str, v32);
          ks.str[ks.str_len] = bkpk;
          break;

          default:
          goto exit;
          break;
        }
      }
      exit:
      if (verbose) printf("\n");
    }


    gettimeofday(&t2, NULL);

    total_time_usec += (u_int64_t) ((u_int64_t) t2.tv_sec * 1000000 + t2.tv_usec) - ((u_int64_t) t1.tv_sec * 1000000 + t1.tv_usec);
  }

  printf("Deserialization perf: %.3f msec total time for %u iterations\n", (double) total_time_usec/1000, repeat);

  for (i = 0; i < num_elements; i++)
    ndpi_term_serializer(&serializer[i]);

  return 0;
}

