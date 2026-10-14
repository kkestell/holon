// Holon keeps one root object in a store. `init` creates a store from an object, `send` runs the
// root object's transition method on an event and commits the result, `inspect` prints the root
// object's state, and `drain` delivers the effects that transitions have queued in the store's
// outbox.
//
// Every value is a blob or a tuple of values. A u64 is an 8-byte little-endian blob. Expressions
// are values too: an expression is a tuple whose first element is an opcode.

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOLON_TESTING
#include <signal.h>
#endif

// Values are immutable once built, so tuples share them freely. Each command is a short process
// that keeps its values until it exits.
typedef struct Value Value;

struct Value {
    int is_tuple;
    size_t length;
    unsigned char *bytes; // Contents of a blob.
    Value **elements;     // Contents of a tuple.
};

// An object is [vtable, state], where vtable is a tuple of method expressions. A method runs with
// the subject [object, args]. Each transition runs the root object's transition method, method 0,
// on an event, and it returns [next_object, effects].
enum { OBJECT_VTABLE, OBJECT_STATE };

enum { RESULT_NEXT_OBJECT, RESULT_EFFECTS };

// A store holds the snapshot ["holon-1", store_id, sequence, object, outbox]. The sequence is 1
// after init and increases with each transition. The outbox lists unacknowledged effects as
// entries [sequence, effect_index, effect], where sequence is the transition's sequence and
// effect_index is the effect's position among its transition's effects. With the store ID, they
// form the effect ID.
enum { STORE_FORMAT, STORE_ID, STORE_SEQUENCE, STORE_OBJECT, STORE_OUTBOX, STORE_FIELDS };

enum { ENTRY_SEQUENCE, ENTRY_EFFECT_INDEX, ENTRY_EFFECT, ENTRY_FIELDS };

#define SNAPSHOT_FORMAT "holon-1"
#define STORE_ID_LENGTH 16

enum Op {
    OP_QUOTE,
    OP_TUPLE,
    OP_KIND,
    OP_LEN,
    OP_GET,
    OP_SET,
    OP_GET_AT,
    OP_SET_AT,
    OP_SLICE,
    OP_CONCAT,
    OP_IF,
    OP_PIPE,
    OP_CALL,
    OP_EVAL,
    OP_EQ,
    OP_ALU,
    OP_TRAP,
    OP_COUNT,
};

struct Opcode {
    const char *name;
    size_t length;
};

// Opcode names and expression lengths, opcode included. `tuple` takes any number of operands.
static const struct Opcode opcodes[OP_COUNT] = {
    [OP_QUOTE] = {"quote", 2},   [OP_TUPLE] = {"tuple", 0},   [OP_KIND] = {"kind", 2},
    [OP_LEN] = {"len", 2},       [OP_GET] = {"get", 2},       [OP_SET] = {"set", 3},
    [OP_GET_AT] = {"get_at", 3}, [OP_SET_AT] = {"set_at", 4}, [OP_SLICE] = {"slice", 4},
    [OP_CONCAT] = {"concat", 3}, [OP_IF] = {"if", 4},         [OP_PIPE] = {"pipe", 3},
    [OP_CALL] = {"call", 4},     [OP_EVAL] = {"eval", 3},     [OP_EQ] = {"eq", 3},
    [OP_ALU] = {"alu", 4},       [OP_TRAP] = {"trap", 2},
};

enum AluOp {
    ALU_ADD,
    ALU_SUB,
    ALU_MUL,
    ALU_DIV,
    ALU_MOD,
    ALU_AND,
    ALU_OR,
    ALU_XOR,
    ALU_SHL,
    ALU_SHR,
    ALU_LT,
    ALU_LE,
    ALU_COUNT,
};

static const char *const alu_names[ALU_COUNT] = {
    [ALU_ADD] = "add", [ALU_SUB] = "sub", [ALU_MUL] = "mul", [ALU_DIV] = "div",
    [ALU_MOD] = "mod", [ALU_AND] = "and", [ALU_OR] = "or",   [ALU_XOR] = "xor",
    [ALU_SHL] = "shl", [ALU_SHR] = "shr", [ALU_LT] = "lt",   [ALU_LE] = "le",
};

static const char *const TYPE_MISMATCH = "trap: TypeMismatch";
static const char *const INDEX_OUT_OF_BOUNDS = "trap: IndexOutOfBounds";
static const char *const MALFORMED_AST = "trap: MalformedAst";
static const char *const DIVIDE_BY_ZERO = "trap: DivideByZero";

static _Noreturn void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

// Fails with message unless condition holds. As a macro, it lets static analysis see that a failed
// check does not return.
#define check(condition, message) ((condition) ? (void)0 : fail(message))

static _Noreturn void fail_io(void) {
    perror("I/O failure; reopen and inspect before retrying; delivery may repeat");
    exit(1);
}

static void *reallocate(void *pointer, size_t count, size_t size) {
    check(count <= SIZE_MAX / size, "allocation too large");
    pointer = realloc(pointer, count ? count * size : 1);
    check(pointer != NULL, "out of memory");
    return pointer;
}

static char *copy_string(const char *text, size_t length) {
    char *copy = reallocate(NULL, length + 1, 1);
    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

static char *join(const char *first, const char *second) {
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    check(first_length < SIZE_MAX - second_length, "path too long");
    char *joined = reallocate(NULL, first_length + second_length + 1, 1);
    memcpy(joined, first, first_length);
    memcpy(joined + first_length, second, second_length + 1);
    return joined;
}

static Value *new_value(int is_tuple, size_t length) {
    Value *value = reallocate(NULL, 1, sizeof(*value));
    *value = (Value){.is_tuple = is_tuple, .length = length};
    if (is_tuple)
        value->elements = reallocate(NULL, length, sizeof(Value *));
    else
        value->bytes = reallocate(NULL, length, 1);
    return value;
}

static Value *new_tuple(size_t length) {
    return new_value(1, length);
}

static Value *new_blob(const void *data, size_t length) {
    Value *value = new_value(0, length);
    if (length)
        memcpy(value->bytes, data, length);
    return value;
}

static Value *new_text(const char *text) {
    return new_blob(text, strlen(text));
}

static void write_u64(unsigned char *bytes, uint64_t n) {
    for (int i = 0; i < 8; i++)
        bytes[i] = (unsigned char)(n >> (8 * i));
}

static uint64_t read_u64(const unsigned char *bytes) {
    uint64_t n = 0;
    for (int i = 7; i >= 0; i--)
        n = (n << 8) | bytes[i];
    return n;
}

static Value *new_u64(uint64_t n) {
    Value *value = new_value(0, 8);
    write_u64(value->bytes, n);
    return value;
}

static Value *new_pair(Value *first, Value *second) {
    Value *pair = new_tuple(2);
    pair->elements[0] = first;
    pair->elements[1] = second;
    return pair;
}

// Returns the u64 in value, failing with error unless value is an 8-byte blob.
static uint64_t decode_u64(Value *value, const char *error) {
    check(!value->is_tuple && value->length == 8, error);
    return read_u64(value->bytes);
}

static uint64_t as_u64(Value *value) {
    return decode_u64(value, TYPE_MISMATCH);
}

static void expect_tuple(Value *value, size_t length) {
    check(value->is_tuple && value->length == length, TYPE_MISMATCH);
}

static void check_index(Value *tuple, uint64_t index) {
    check(tuple->is_tuple, TYPE_MISMATCH);
    check(index < tuple->length, INDEX_OUT_OF_BOUNDS);
}

static Value *tuple_element(Value *tuple, uint64_t index) {
    check_index(tuple, index);
    return tuple->elements[index];
}

static void expect_object(Value *value) {
    expect_tuple(value, 2);
    check(value->elements[OBJECT_VTABLE]->is_tuple, TYPE_MISMATCH);
}

// A root object needs a transition method, so its vtable is nonempty.
static void expect_root_object(Value *value) {
    expect_object(value);
    check(value->elements[OBJECT_VTABLE]->length > 0, TYPE_MISMATCH);
}

static int equal(Value *first, Value *second) {
    if (first == second)
        return 1;
    if (first->is_tuple != second->is_tuple || first->length != second->length)
        return 0;
    if (!first->is_tuple)
        return memcmp(first->bytes, second->bytes, first->length) == 0;
    for (size_t i = 0; i < first->length; i++)
        if (!equal(first->elements[i], second->elements[i]))
            return 0;
    return 1;
}

// Returns count elements or bytes of value, beginning at start.
static Value *slice(Value *value, uint64_t start, uint64_t count) {
    check(start <= value->length && count <= value->length - start, INDEX_OUT_OF_BOUNDS);
    Value *result = new_value(value->is_tuple, (size_t)count);
    if (value->is_tuple)
        memcpy(result->elements, value->elements + start, count * sizeof(Value *));
    else
        memcpy(result->bytes, value->bytes + start, count);
    return result;
}

static Value *copy_value(Value *value) {
    return slice(value, 0, value->length);
}

static Value *concat(Value *first, Value *second) {
    check(first->is_tuple == second->is_tuple, TYPE_MISMATCH);
    check(second->length <= SIZE_MAX - first->length, "value too large");
    Value *result = new_value(first->is_tuple, first->length + second->length);
    if (first->is_tuple) {
        memcpy(result->elements, first->elements, first->length * sizeof(Value *));
        memcpy(result->elements + first->length, second->elements,
               second->length * sizeof(Value *));
    } else {
        memcpy(result->bytes, first->bytes, first->length);
        memcpy(result->bytes + first->length, second->bytes, second->length);
    }
    return result;
}

static Value *replace_element(Value *tuple, uint64_t index, Value *element) {
    check_index(tuple, index);
    Value *copy = copy_value(tuple);
    copy->elements[index] = element;
    return copy;
}

// A path is a blob of u64 tuple indices. The source text path[1, 0] parses to one.
static void expect_path(Value *path) {
    check(!path->is_tuple && path->length % 8 == 0, MALFORMED_AST);
}

static Value *path_get(Value *value, Value *path) {
    for (size_t offset = 0; offset < path->length; offset += 8)
        value = tuple_element(value, read_u64(path->bytes + offset));
    return value;
}

// Returns a copy of value with the value at path, from offset onward, replaced by replacement.
static Value *path_set(Value *value, Value *path, size_t offset, Value *replacement) {
    if (offset == path->length)
        return replacement;
    uint64_t index = read_u64(path->bytes + offset);
    Value *child = path_set(tuple_element(value, index), path, offset + 8, replacement);
    return replace_element(value, index, child);
}

static int is_printable(Value *value) {
    for (size_t i = 0; i < value->length; i++)
        if (value->bytes[i] < 32 || value->bytes[i] > 126)
            return 0;
    return 1;
}

static void print_blob(FILE *file, Value *value) {
    if (is_printable(value)) {
        fputc('"', file);
        for (size_t i = 0; i < value->length; i++) {
            unsigned char c = value->bytes[i];
            if (c == '"' || c == '\\')
                fputc('\\', file);
            fputc(c, file);
        }
        fputc('"', file);
    } else if (value->length == 8) {
        fprintf(file, "%" PRIu64, read_u64(value->bytes));
    } else {
        fputs("x\"", file);
        for (size_t i = 0; i < value->length; i++)
            fprintf(file, "%02x", value->bytes[i]);
        fputc('"', file);
    }
}

// Prints value in the syntax the parser reads. A blob prints as text when every byte is printable
// ASCII, otherwise as a u64 when it is 8 bytes long, otherwise as hex.
static void print_value(FILE *file, Value *value) {
    if (!value->is_tuple) {
        print_blob(file, value);
        return;
    }
    fputc('[', file);
    for (size_t i = 0; i < value->length; i++) {
        if (i)
            fputs(", ", file);
        print_value(file, value->elements[i]);
    }
    fputc(']', file);
}

// The parser's position in the input text.
static const char *cursor;

// Skips whitespace and comments, which run from # to the end of the line.
static void skip_space(void) {
    for (;;) {
        while (isspace((unsigned char)*cursor))
            cursor++;
        if (*cursor != '#')
            return;
        while (*cursor && *cursor != '\n')
            cursor++;
    }
}

// Consumes c if it is the next character after any whitespace.
static int accept(char c) {
    skip_space();
    if (*cursor != c)
        return 0;
    cursor++;
    return 1;
}

static void expect_char(char c) {
    check(accept(c), "parse error: unexpected token");
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void append_byte(Value *value, unsigned char byte) {
    check(value->length < SIZE_MAX, "input too large");
    value->bytes = reallocate(value->bytes, value->length + 1, 1);
    value->bytes[value->length++] = byte;
}

static void append_element(Value *tuple, Value *element) {
    check(tuple->length < SIZE_MAX, "input too large");
    tuple->elements = reallocate(tuple->elements, tuple->length + 1, sizeof(Value *));
    tuple->elements[tuple->length++] = element;
}

static Value *parse_value(void);

// Parses the rest of a tuple such as [1, "a"], after the opening bracket.
static Value *parse_tuple(void) {
    Value *tuple = new_tuple(0);
    while (!accept(']')) {
        append_element(tuple, parse_value());
        if (accept(']'))
            break;
        expect_char(',');
    }
    return tuple;
}

// Parses the rest of a hex blob such as x"00ff", after the opening x".
static Value *parse_hex(void) {
    Value *value = new_blob("", 0);
    while (*cursor && *cursor != '"') {
        int high = hex_digit(cursor[0]);
        int low = hex_digit(cursor[1]);
        check(high >= 0 && low >= 0, "parse error: invalid hex blob");
        append_byte(value, (unsigned char)(16 * high + low));
        cursor += 2;
    }
    expect_char('"');
    return value;
}

// Parses the rest of a text string such as "a\"b", after the opening quote. A backslash escapes a
// quote or a backslash.
static Value *parse_string(void) {
    Value *value = new_blob("", 0);
    while (*cursor && *cursor != '"') {
        if (*cursor == '\\') {
            cursor++;
            check(*cursor == '\\' || *cursor == '"', "parse error: invalid escape");
        }
        append_byte(value, (unsigned char)*cursor);
        cursor++;
    }
    expect_char('"');
    return value;
}

static Value *parse_integer(void) {
    uint64_t n = 0;
    while (isdigit((unsigned char)*cursor)) {
        unsigned digit = (unsigned)(*cursor - '0');
        check(n <= (UINT64_MAX - digit) / 10, "parse error: integer overflow");
        n = n * 10 + digit;
        cursor++;
    }
    return new_u64(n);
}

// Parses the index list of a path such as path[1, 0], after the word path.
static Value *parse_path(void) {
    skip_space();
    check(*cursor == '[', "parse error: expected path indices");
    Value *indices = parse_value();
    check(indices->length <= SIZE_MAX / 8, "input too large");
    Value *path = new_value(0, indices->length * 8);
    for (size_t i = 0; i < indices->length; i++)
        write_u64(path->bytes + 8 * i, as_u64(indices->elements[i]));
    return path;
}

static int word_is(const char *word, size_t length, const char *name) {
    return strlen(name) == length && !memcmp(word, name, length);
}

// Parses a path, an opcode name, an ALU operation name, true, or false. Names and booleans parse
// as u64s.
static Value *parse_word(void) {
    const char *word = cursor;
    while (isalnum((unsigned char)*cursor) || *cursor == '_')
        cursor++;
    size_t length = (size_t)(cursor - word);
    if (word_is(word, length, "path"))
        return parse_path();
    for (int op = 0; op < OP_COUNT; op++)
        if (word_is(word, length, opcodes[op].name))
            return new_u64((uint64_t)op);
    for (int op = 0; op < ALU_COUNT; op++)
        if (word_is(word, length, alu_names[op]))
            return new_u64((uint64_t)op);
    if (word_is(word, length, "true"))
        return new_u64(1);
    if (word_is(word, length, "false"))
        return new_u64(0);
    fail("parse error: expected value");
}

static Value *parse_value(void) {
    skip_space();
    if (accept('['))
        return parse_tuple();
    if (cursor[0] == 'x' && cursor[1] == '"') {
        cursor += 2;
        return parse_hex();
    }
    if (*cursor == '"') {
        cursor++;
        return parse_string();
    }
    if (isdigit((unsigned char)*cursor))
        return parse_integer();
    return parse_word();
}

static Value *parse_text(const char *text) {
    cursor = text;
    Value *value = parse_value();
    skip_space();
    check(!*cursor, "parse error: trailing input");
    return value;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        fail_io();
    if (fseek(file, 0, SEEK_END))
        fail_io();
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET))
        fail_io();
    check((uintmax_t)length < SIZE_MAX, "input too large");
    size_t size = (size_t)length;
    char *text = reallocate(NULL, size + 1, 1);
    if (fread(text, 1, size, file) != size || fclose(file))
        fail_io();
    check(!memchr(text, 0, size), "parse error: NUL in input");
    text[size] = 0;
    return text;
}

static Value *parse_file(const char *path) {
    char *text = read_file(path);
    Value *value = parse_text(text);
    free(text);
    return value;
}

// Arithmetic wraps modulo 2^64, shifts by 64 or more give 0, and comparisons give 1 or 0.
static uint64_t apply_alu(enum AluOp op, uint64_t a, uint64_t b) {
    switch (op) {
    case ALU_ADD:
        return a + b;
    case ALU_SUB:
        return a - b;
    case ALU_MUL:
        return a * b;
    case ALU_DIV:
        check(b != 0, DIVIDE_BY_ZERO);
        return a / b;
    case ALU_MOD:
        check(b != 0, DIVIDE_BY_ZERO);
        return a % b;
    case ALU_AND:
        return a & b;
    case ALU_OR:
        return a | b;
    case ALU_XOR:
        return a ^ b;
    case ALU_SHL:
        return b < 64 ? a << b : 0;
    case ALU_SHR:
        return b < 64 ? a >> b : 0;
    case ALU_LT:
        return a < b;
    case ALU_LE:
        return a <= b;
    case ALU_COUNT:
        break;
    }
    fail(MALFORMED_AST);
}

// Evaluates expression against subject, the value that `get` and `set` read and update.
// `if`, `pipe`, `call`, and `eval` finish by evaluating another expression, which the loop does
// in place so that tail calls run in constant stack space. Operands follow the opcode at
// operand[0].
static Value *evaluate(Value *subject, Value *expression) {
    for (;;) {
        check(expression->is_tuple && expression->length > 0, MALFORMED_AST);
        uint64_t op = decode_u64(expression->elements[0], MALFORMED_AST);
        check(op < OP_COUNT, MALFORMED_AST);
        check(op == OP_TUPLE || expression->length == opcodes[op].length, MALFORMED_AST);
        Value **operand = expression->elements;
        switch ((enum Op)op) {
        // [quote, value] returns value as a literal.
        case OP_QUOTE:
            return operand[1];
        // [tuple, a, b, ...] returns a tuple of the evaluated operands.
        case OP_TUPLE: {
            Value *tuple = new_tuple(expression->length - 1);
            for (size_t i = 1; i < expression->length; i++)
                tuple->elements[i - 1] = evaluate(subject, operand[i]);
            return tuple;
        }
        // [kind, value] returns 1 for a tuple and 0 for a blob.
        case OP_KIND:
            return new_u64((uint64_t)evaluate(subject, operand[1])->is_tuple);
        // [len, value] returns the number of elements or bytes.
        case OP_LEN:
            return new_u64(evaluate(subject, operand[1])->length);
        // [get, path] returns the value at a literal path in the subject.
        case OP_GET:
            expect_path(operand[1]);
            return path_get(subject, operand[1]);
        // [set, path, replacement] returns the subject with the value at a literal path replaced.
        // The path must exist before replacement is evaluated.
        case OP_SET: {
            expect_path(operand[1]);
            path_get(subject, operand[1]);
            Value *replacement = evaluate(subject, operand[2]);
            return path_set(subject, operand[1], 0, replacement);
        }
        // [get_at, value, index] returns a tuple element, or a byte as a 1-byte blob.
        case OP_GET_AT: {
            Value *value = evaluate(subject, operand[1]);
            uint64_t index = as_u64(evaluate(subject, operand[2]));
            check(index < value->length, INDEX_OUT_OF_BOUNDS);
            if (value->is_tuple)
                return value->elements[index];
            return new_blob(value->bytes + index, 1);
        }
        // [set_at, tuple, index, replacement] returns tuple with the element at index replaced.
        case OP_SET_AT: {
            Value *tuple = evaluate(subject, operand[1]);
            check(tuple->is_tuple, TYPE_MISMATCH);
            uint64_t index = as_u64(evaluate(subject, operand[2]));
            check_index(tuple, index);
            Value *replacement = evaluate(subject, operand[3]);
            return replace_element(tuple, index, replacement);
        }
        // [slice, value, start, count] returns count elements or bytes beginning at start.
        case OP_SLICE: {
            Value *value = evaluate(subject, operand[1]);
            uint64_t start = as_u64(evaluate(subject, operand[2]));
            uint64_t count = as_u64(evaluate(subject, operand[3]));
            return slice(value, start, count);
        }
        // [concat, first, second] joins two tuples or two blobs.
        case OP_CONCAT: {
            Value *first = evaluate(subject, operand[1]);
            Value *second = evaluate(subject, operand[2]);
            return concat(first, second);
        }
        // [if, condition, then, else] evaluates then when condition is 1 and else when it is 0.
        case OP_IF: {
            uint64_t condition = as_u64(evaluate(subject, operand[1]));
            check(condition <= 1, TYPE_MISMATCH);
            expression = condition ? operand[2] : operand[3];
            continue;
        }
        // [pipe, first, second] evaluates second with the result of first as its subject.
        case OP_PIPE:
            subject = evaluate(subject, operand[1]);
            expression = operand[2];
            continue;
        // [call, object, index, args] runs the method at index in the object's vtable with the
        // subject [object, args].
        case OP_CALL: {
            Value *object = evaluate(subject, operand[1]);
            expect_object(object);
            uint64_t index = as_u64(evaluate(subject, operand[2]));
            Value *method = tuple_element(object->elements[OBJECT_VTABLE], index);
            Value *args = evaluate(subject, operand[3]);
            subject = new_pair(object, args);
            expression = method;
            continue;
        }
        // [eval, subject, expression] evaluates both operands, then evaluates the resulting
        // expression against the resulting subject.
        case OP_EVAL: {
            Value *next_subject = evaluate(subject, operand[1]);
            expression = evaluate(subject, operand[2]);
            subject = next_subject;
            continue;
        }
        // [eq, first, second] returns 1 when the values are equal and 0 otherwise.
        case OP_EQ: {
            Value *first = evaluate(subject, operand[1]);
            Value *second = evaluate(subject, operand[2]);
            return new_u64((uint64_t)equal(first, second));
        }
        // [alu, operation, a, b] applies a literal ALU operation to two u64s.
        case OP_ALU: {
            uint64_t alu_op = decode_u64(operand[1], MALFORMED_AST);
            check(alu_op < ALU_COUNT, MALFORMED_AST);
            uint64_t a = as_u64(evaluate(subject, operand[2]));
            uint64_t b = as_u64(evaluate(subject, operand[3]));
            return new_u64(apply_alu((enum AluOp)alu_op, a, b));
        }
        // [trap, reason] prints reason and exits with an error.
        case OP_TRAP: {
            Value *reason = evaluate(subject, operand[1]);
            fputs("trap: ", stderr);
            print_value(stderr, reason);
            fputc('\n', stderr);
            exit(1);
        }
        case OP_COUNT:
            fail(MALFORMED_AST);
        }
    }
}

// In test builds, stops the process at the point named by HOLON_STOP_AT so that a test can kill
// it there.
static void checkpoint(const char *name) {
#ifdef HOLON_TESTING
    const char *point = getenv("HOLON_STOP_AT");
    if (point && !strcmp(point, name)) {
        fprintf(stderr, "stopped: %s\n", name);
        fflush(stderr);
        raise(SIGSTOP);
    }
#else
    (void)name;
#endif
}

static void sync_file(int fd) {
#ifdef __APPLE__
    if (fcntl(fd, F_FULLFSYNC) < 0)
        fail_io();
#else
    if (fsync(fd))
        fail_io();
#endif
}

static char *parent_directory(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return copy_string(".", 1);
    if (slash == path)
        return copy_string("/", 1);
    return copy_string(path, (size_t)(slash - path));
}

// Commits snapshot by replacing the store at path. The snapshot is written and synced to a
// temporary file, which is renamed over the store, and then the directory is synced so the rename
// is durable. An interruption leaves either the old or the new snapshot in place.
static void commit_snapshot(const char *path, Value *snapshot) {
    char *directory_path = parent_directory(path);
    int directory = open(directory_path, O_RDONLY);
    if (directory < 0)
        fail_io();
    char *temp_path = join(path, ".tmp.XXXXXX");
    int fd = mkstemp(temp_path);
    if (fd < 0)
        fail_io();
    FILE *file = fdopen(fd, "w");
    if (!file)
        fail_io();
    print_value(file, snapshot);
    fputc('\n', file);
    if (fflush(file) || ferror(file))
        fail_io();
    sync_file(fd);
    if (fclose(file))
        fail_io();
    checkpoint("before-rename");
    if (rename(temp_path, path))
        fail_io();
    checkpoint("after-rename");
    sync_file(directory);
    if (close(directory))
        fail_io();
    free(temp_path);
    free(directory_path);
}

static Value *load_snapshot(const char *path) {
    Value *snapshot = parse_file(path);
    expect_tuple(snapshot, STORE_FIELDS);
    check(equal(snapshot->elements[STORE_FORMAT], new_text(SNAPSHOT_FORMAT)),
          "invalid snapshot format");
    Value *id = snapshot->elements[STORE_ID];
    check(!id->is_tuple && id->length == STORE_ID_LENGTH, "invalid store ID");
    uint64_t sequence = as_u64(snapshot->elements[STORE_SEQUENCE]);
    check(sequence > 0, "invalid sequence");
    expect_root_object(snapshot->elements[STORE_OBJECT]);
    Value *outbox = snapshot->elements[STORE_OUTBOX];
    check(outbox->is_tuple, "invalid outbox");

    // Entries come from transitions after init and appear in increasing effect ID order.
    uint64_t previous_sequence = 0;
    uint64_t previous_index = 0;
    for (size_t i = 0; i < outbox->length; i++) {
        Value *entry = outbox->elements[i];
        expect_tuple(entry, ENTRY_FIELDS);
        uint64_t entry_sequence = as_u64(entry->elements[ENTRY_SEQUENCE]);
        uint64_t effect_index = as_u64(entry->elements[ENTRY_EFFECT_INDEX]);
        int in_order = entry_sequence > previous_sequence ||
                       (entry_sequence == previous_sequence && effect_index > previous_index);
        check(entry_sequence > 1 && entry_sequence <= sequence && in_order, "invalid effect ID");
        previous_sequence = entry_sequence;
        previous_index = effect_index;
    }
    return snapshot;
}

static Value *random_id(void) {
    unsigned char id[STORE_ID_LENGTH];
    FILE *random = fopen("/dev/urandom", "rb");
    if (!random)
        fail_io();
    if (fread(id, 1, sizeof(id), random) != sizeof(id) || fclose(random))
        fail_io();
    return new_blob(id, sizeof(id));
}

static Value *new_snapshot(Value *object) {
    expect_root_object(object);
    Value *snapshot = new_tuple(STORE_FIELDS);
    snapshot->elements[STORE_FORMAT] = new_text(SNAPSHOT_FORMAT);
    snapshot->elements[STORE_ID] = random_id();
    snapshot->elements[STORE_SEQUENCE] = new_u64(1);
    snapshot->elements[STORE_OBJECT] = object;
    snapshot->elements[STORE_OUTBOX] = new_tuple(0);
    return snapshot;
}

// Runs the root object's transition method with the subject [object, event]. The method's result
// [next_object, effects] supplies the next root object, and each effect joins the outbox as an
// entry.
static Value *transition(Value *snapshot, Value *event) {
    Value *object = snapshot->elements[STORE_OBJECT];
    Value *method = object->elements[OBJECT_VTABLE]->elements[0];
    Value *result = evaluate(new_pair(object, event), method);
    expect_tuple(result, 2);
    expect_root_object(result->elements[RESULT_NEXT_OBJECT]);
    Value *effects = result->elements[RESULT_EFFECTS];
    check(effects->is_tuple, TYPE_MISMATCH);

    uint64_t sequence = as_u64(snapshot->elements[STORE_SEQUENCE]);
    check(sequence < UINT64_MAX, "sequence exhausted");
    Value *next_sequence = new_u64(sequence + 1);
    Value *entries = new_tuple(effects->length);
    for (size_t i = 0; i < effects->length; i++) {
        Value *entry = new_tuple(ENTRY_FIELDS);
        entry->elements[ENTRY_SEQUENCE] = next_sequence;
        entry->elements[ENTRY_EFFECT_INDEX] = new_u64(i);
        entry->elements[ENTRY_EFFECT] = effects->elements[i];
        entries->elements[i] = entry;
    }

    Value *next = copy_value(snapshot);
    next->elements[STORE_SEQUENCE] = next_sequence;
    next->elements[STORE_OBJECT] = result->elements[RESULT_NEXT_OBJECT];
    next->elements[STORE_OUTBOX] = concat(snapshot->elements[STORE_OUTBOX], entries);
    return next;
}

// Delivers an outbox entry's effect by printing "effect: EFFECT_ID EFFECT", where EFFECT_ID is
// STORE_ID:SEQUENCE:EFFECT_INDEX with the store ID in hex.
static void deliver(Value *store_id, Value *entry) {
    fputs("effect: ", stdout);
    for (size_t i = 0; i < store_id->length; i++)
        printf("%02x", store_id->bytes[i]);
    uint64_t sequence = as_u64(entry->elements[ENTRY_SEQUENCE]);
    uint64_t effect_index = as_u64(entry->elements[ENTRY_EFFECT_INDEX]);
    printf(":%" PRIu64 ":%" PRIu64 " ", sequence, effect_index);
    print_value(stdout, entry->elements[ENTRY_EFFECT]);
    putchar('\n');
    if (fflush(stdout) || ferror(stdout))
        fail_io();
}

// Delivers each effect in the outbox, then acknowledges it by committing the snapshot with the
// remaining entries. An interruption between the two repeats the delivery on the next drain, so
// every effect is delivered at least once.
static void drain(const char *path, Value *snapshot) {
    Value *outbox = snapshot->elements[STORE_OUTBOX];
    for (size_t i = 0; i < outbox->length; i++) {
        deliver(snapshot->elements[STORE_ID], outbox->elements[i]);
        checkpoint("after-delivery");
        Value *next = copy_value(snapshot);
        next->elements[STORE_OUTBOX] = slice(outbox, i + 1, outbox->length - i - 1);
        commit_snapshot(path, next);
    }
}

static void print_state(Value *snapshot) {
    print_value(stdout, snapshot->elements[STORE_OBJECT]->elements[OBJECT_STATE]);
    putchar('\n');
    if (fflush(stdout) || ferror(stdout))
        fail_io();
}

enum Command { COMMAND_INIT, COMMAND_SEND, COMMAND_INSPECT, COMMAND_DRAIN };

static enum Command parse_command(int argc, char **argv) {
    check(argc >= 3,
          "usage: holon init STORE OBJECT | send STORE EVENT | inspect STORE | drain STORE");
    const char *name = argv[1];
    if (!strcmp(name, "init") && argc == 4)
        return COMMAND_INIT;
    if (!strcmp(name, "send") && argc == 4)
        return COMMAND_SEND;
    if (!strcmp(name, "inspect") && argc == 3)
        return COMMAND_INSPECT;
    if (!strcmp(name, "drain") && argc == 3)
        return COMMAND_DRAIN;
    fail("invalid command or arguments");
}

// Takes an exclusive lock on the store so that commands on it run one at a time.
static int lock_store(const char *path) {
    char *lock_path = join(path, ".lock");
    int lock = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock < 0)
        fail_io();
    if (flock(lock, LOCK_EX))
        fail_io();
    free(lock_path);
    return lock;
}

static void check_store_absent(const char *path) {
    struct stat st;
    check(lstat(path, &st) != 0, "store already exists");
    if (errno != ENOENT)
        fail_io();
}

int main(int argc, char **argv) {
    enum Command command = parse_command(argc, argv);
    const char *path = argv[2];
    Value *input = NULL;
    if (command == COMMAND_INIT)
        input = parse_file(argv[3]);
    else if (command == COMMAND_SEND)
        input = parse_text(argv[3]);

    int lock = lock_store(path);
    switch (command) {
    case COMMAND_INIT: {
        check_store_absent(path);
        Value *snapshot = new_snapshot(input);
        commit_snapshot(path, snapshot);
        print_state(snapshot);
        break;
    }
    case COMMAND_SEND: {
        Value *snapshot = transition(load_snapshot(path), input);
        commit_snapshot(path, snapshot);
        print_state(snapshot);
        break;
    }
    case COMMAND_INSPECT:
        print_state(load_snapshot(path));
        break;
    case COMMAND_DRAIN:
        drain(path, load_snapshot(path));
        break;
    }
    if (close(lock))
        fail_io();
    return 0;
}
