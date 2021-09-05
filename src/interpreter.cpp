#include "interpreter.hpp"

#include <fmt/core.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "errors.hpp"
#include "objects.hpp"
#include "platform/platform.hpp"
#include "util.hpp"

using fmt::format;
using std::filesystem::path;

InterpreterState IS;

inline bool can_be_a_part_of_symbol(char ch) {
  return isalpha(ch) || ch == '+' || ch == '-' || ch == '=' || ch == '-' ||
         ch == '*' || ch == '/' || ch == '>' || ch == '<' || ch == '?';
}

inline char next_char() {
  ++IS.text_pos;
  return IS.text[IS.text_pos];
}

inline char get_char() { return IS.text[IS.text_pos]; }

inline void skip_char() {
  ++IS.col;
  ++IS.text_pos;
}

inline void consume_char(char ch) {
  ++IS.col;
  if (get_char() == ch) {
    ++IS.text_pos;
    return;
  }
  error_msg(format("Expected {} but found {}\n", ch, *IS.text));
  exit(1);
}

inline void set_symbol(std::string const &key, Object *value) {
  IS.symtable->map[key] = value;
}

Object *get_symbol(std::string &key) {
  SymTable *ltable = IS.symtable;
  while (true) {
    bool present = ltable->map.find(key) != ltable->map.end();
    if (present) {
      return ltable->map[key];
    }
    // Global table
    if (ltable->prev == nullptr) {
      return nil_obj;
    }
    ltable = ltable->prev;
  }
}

// TODO: Add limit to the depth of the symbol table (to prevent stack overflows)
void enter_scope() {
  SymTable *new_scope = new SymTable();
  new_scope->prev = IS.symtable;
  IS.symtable = new_scope;
}

void enter_scope_with(SymVars vars) {
  SymTable *new_scope = new SymTable();
  new_scope->map = vars;
  new_scope->prev = IS.symtable;
  IS.symtable = new_scope;
}

void exit_scope() {
  // TODO: assert that we there is a parent table
  auto *prev = IS.symtable->prev;
  delete IS.symtable;
  IS.symtable = prev;
}

Object *read_str() {
  auto *svalue = new std::string("");
  consume_char('"');
  char ch = get_char();
  while (IS.text_pos < IS.text_len && ch != '"') {
    svalue->push_back(ch);
    ch = next_char();
  }
  consume_char('"');
  return create_str_obj(svalue);
}

Object *read_sym() {
  auto *svalue = new std::string("");
  char ch = get_char();
  while (IS.text_pos < IS.text_len && can_be_a_part_of_symbol(ch)) {
    svalue->push_back(ch);
    ch = next_char();
  }
  return create_sym_obj(svalue);
}

Object *read_num() {
  char ch = get_char();
  static char buf[1024];
  int buf_len = 0;
  while (IS.text_pos < IS.text_len && isdigit(ch)) {
    buf[buf_len] = ch;
    ch = next_char();
    ++buf_len;
  }
  buf[buf_len] = '\0';
  int v = std::stoi(buf);
  return create_num_obj(v);
}

Object *read_expr();

Object *read_list(bool literal = false) {
  Object *res = create_list_obj();
  if (literal) {
    res->flags |= OF_LIST_LITERAL;
  }
  consume_char('(');
  while (get_char() != ')') {
    if (IS.text_pos >= IS.text_len) {
      eof_error();
      exit(1);
      return nullptr;
    }
    auto *e = read_expr();
    list_append_inplace(res, e);
  }
  consume_char(')');
  return res;
}

Object *read_expr() {
  char ch = get_char();
  if (IS.text_pos >= IS.text_len) return nil_obj;
  switch (ch) {
    case ' ': {
      skip_char();
      return read_expr();
    } break;
    case '\n':
    case '\r': {
      // TODO: Count the skipped lines
      skip_char();
      ++IS.line;
      IS.col = 0;
      return read_expr();
    } break;
    case ';': {
      // TODO: Count the skipped lines
      // skip until the end of the line
      while (get_char() != '\n') {
        skip_char();
      }
      skip_char();
      return read_expr();
    } break;
    case '(': {
      return read_list();
    } break;
    case '\'': {
      skip_char();
      return read_list(true);
    } break;
    case '"': {
      return read_str();
    } break;
    case '.':
      skip_char();
      return dot_obj;
    default: {
      if (isdigit(ch)) {
        return read_num();
      }
      if (can_be_a_part_of_symbol(ch)) {
        return read_sym();
      }
      printf("Invalid character: %c (%i)\n", ch, ch);
      exit(1);
      return nullptr;
    }
  }
}

Object *eval_expr(Object *expr);

Object *add_objects(Object *expr) {
  auto *l = expr->val.l_value;
  int elems_len = l->size();
  int args_len = elems_len - 1;
  if (args_len < 2) {
    printf("Add (+) operator can't have less than two arguments\n");
    return nil_obj;
  }
  Object *add_res = eval_expr(l->at(1));
  int arg_idx = 2;
  while (arg_idx < elems_len) {
    auto *operand = eval_expr(l->at(arg_idx));
    // actually add objects
    add_res = add_two_objects(add_res, operand);
    ++arg_idx;
  }
  return add_res;
}

Object *sub_objects(Object *expr) {
  auto *l = expr->val.l_value;
  int elems_len = l->size();
  int args_len = elems_len - 1;
  if (args_len < 2) {
    printf("Subtraction (+) operator can't have less than two arguments\n");
    return nil_obj;
  }
  Object *res = eval_expr(l->at(1));
  int arg_idx = 2;
  while (arg_idx < elems_len) {
    auto *operand = eval_expr(l->at(arg_idx));
    res = sub_two_objects(res, operand);
    ++arg_idx;
  }
  return res;
}

////////////////////////////////////////////////////
// Built-ins
////////////////////////////////////////////////////

void create_builtin_function_and_save(char const *name, Builtin handler) {
  set_symbol(name, create_builtin_fobj(name, handler));
}

Object *setq_builtin(Object *expr) {
  auto *l = expr->val.l_value;
  int elems_len = l->size();
  int args_len = elems_len - 1;
  if (args_len != 2) {
    printf("setq takes exactly two arguments, %i were given\n", args_len);
    return nil_obj;
  }
  Object *symname = l->at(1);
  Object *symvalue = eval_expr(l->at(2));
  set_symbol(*symname->val.s_value, symvalue);
  return nil_obj;
}

Object *print_builtin(Object *expr) {
  auto *l = expr->val.l_value;
  int elems_len = l->size();
  int arg_idx = 1;
  while (arg_idx < elems_len) {
    auto *arg = eval_expr(l->at(arg_idx));
    auto *sobj = obj_to_string(arg);
    // TODO: Handle escape sequences
    printf("%s", sobj->val.s_value->data());
    ++arg_idx;
  }
  printf("\n");
  return nil_obj;
}

Object *defun_builtin(Object *expr) {
  auto *l = expr->val.l_value;
  int elems_len = l->size();
  if (elems_len < 3) {
    printf("Function should have an argument list and a body\n");
    return nil_obj;
  }
  auto *fundef_list = l->at(1);
  // parse function definition list
  if (fundef_list->type != ObjType::List) {
    printf("Function definition list should be a list");
    return nil_obj;
  }
  auto *funobj = new_object(ObjType::Function);
  auto *fundef_list_v = fundef_list->val.l_value;
  auto *funname = fundef_list_v->at(0)->val.s_value;
  funobj->val.f_value.funargs = fundef_list;
  funobj->val.f_value.funbody = expr;
  set_symbol(*funname, funobj);
  return funobj;
}

Object *lambda_builtin(Object *expr) {
  auto *l = expr->val.l_value;
  int elems_len = l->size();
  if (elems_len < 3) {
    printf("Lambdas should have an argument list and a body\n");
    return nil_obj;
  }
  // parse function definition list
  auto *fundef_list = l->at(1);
  if (fundef_list->type != ObjType::List) {
    printf("First paremeter of lambda() should be a list");
    return nil_obj;
  }
  auto *funobj = new_object(ObjType::Function);
  funobj->flags |= OF_LAMBDA;
  funobj->val.f_value.funargs = fundef_list;
  funobj->val.f_value.funbody = expr;
  return funobj;
}

Object *if_builtin(Object *expr) {
  auto *l = expr->val.l_value;
  if (l->size() != 4) {
    error_msg(
        format("if takes exactly 3 arguments: condition, then, and else "
               "blocks. The function was given {} arguments instead\n",
               l->size()));
    return nil_obj;
  }
  auto *condition = l->at(1);
  auto *then_expr = l->at(2);
  auto *else_expr = l->at(3);
  if (is_truthy(eval_expr(condition))) {
    return eval_expr(then_expr);
  } else {
    return eval_expr(else_expr);
  }
}

Object *binary_builtin(Object *expr, char const *name,
                       BinaryObjOpHandler handler) {
  auto *l = expr->val.l_value;
  size_t given_args = l->size() - 1;
  if (l->size() != 3) {
    error_msg(format("{} takes exactly 2 operands, {} was given\n", name,
                     given_args));
    return nil_obj;
  }
  auto *left_op = eval_expr(l->at(1));
  auto *right_op = eval_expr(l->at(2));
  return handler(left_op, right_op);
}

Object *equal_builtin(Object *expr) {
  return binary_builtin(expr, "=", objects_equal);
}

Object *gt_builtin(Object *expr) {
  return binary_builtin(expr, ">", objects_gt);
}

Object *lt_builtin(Object *expr) {
  return binary_builtin(expr, "<", objects_lt);
}

Object *div_objects_builtin(Object *expr) {
  return binary_builtin(expr, "/", objects_div);
}

Object *mul_objects_builtin(Object *expr) {
  return binary_builtin(expr, "*", objects_mul);
}

Object *pow_objects_builtin(Object *expr) {
  return binary_builtin(expr, "**", objects_pow);
}

Object *car_builtin(Object *expr) {
  auto *list_to_operate_on = eval_expr(list_index(expr, 1));
  if (!is_list(list_to_operate_on)) {
    auto *s = obj_to_string_bare(list_to_operate_on);
    printf("car only operates on lists, got %s\n", s->data());
    delete s;
    return nil_obj;
  }
  if (list_length(list_to_operate_on) < 1) return nil_obj;
  return list_index(list_to_operate_on, 0);
}

Object *cadr_builtin(Object *expr) {
  auto *list_to_operate_on = eval_expr(list_index(expr, 1));
  if (!is_list(list_to_operate_on)) {
    auto *s = obj_to_string_bare(list_to_operate_on);
    printf("cadr only operates on lists, got %s\n", s->data());
    delete s;
    return nil_obj;
  }
  if (list_length(list_to_operate_on) < 2) return nil_obj;
  return list_index(list_to_operate_on, 1);
}

Object *cdr_builtin(Object *expr) {
  // currently creating a new list object for every cdr call. Maybe store as a
  // linked list instead and return a pointer to the next of the head so that
  // this call is only O(1)?
  auto *new_list = create_list_obj();
  auto *list_to_operate_on = eval_expr(list_index(expr, 1));
  if (!is_list(list_to_operate_on)) {
    auto *s = obj_to_string_bare(list_to_operate_on);
    printf("cdr only operates on lists, got %s\n", s->data());
    delete s;
    return nil_obj;
  }
  if (list_to_operate_on->type != ObjType::List) {
    printf("cdr can only operate on lists\n");
    return nil_obj;
  }
  if (list_length(list_to_operate_on) < 1) return list_to_operate_on;
  for (size_t i = 1; i < list_length(list_to_operate_on); ++i) {
    auto *evaluated_item = list_index(list_to_operate_on, i);
    list_append_inplace(new_list, evaluated_item);
  }
  new_list->flags |= OF_EVALUATED;
  return new_list;
}

Object *cond_builtin(Object *expr) {
  // sequentually check every provided condition
  // and if one of them is true, return the provided value
  if (list_length(expr) < 2) {
    printf("cond requires at least one condition pair argument");
    return nil_obj;
  }
  for (size_t cond_idx = 1; cond_idx < list_length(expr); ++cond_idx) {
    auto *cond_pair = list_index(expr, cond_idx);
    auto *cond_expr = list_index(cond_pair, 0);
    auto *cond_evaluated = eval_expr(cond_expr);
    // this is an "else" branch, and so just return the value since there was no
    // matches before
    bool otherwise_branch = cond_evaluated == else_obj;
    if (otherwise_branch || is_truthy(cond_evaluated)) {
      auto *res = eval_expr(list_index(cond_pair, 1));
      return res;
    }
  }
  return nil_obj;
}

inline bool check_builtin_n_params(char const *bname, Object const *expr,
                                   size_t n) {
  size_t got_params = list_length(expr) - 1;
  if (got_params != n) {
    error_builtin_arg_mismatch_function("timeit", 1, expr);
    return false;
  }
  return true;
}

inline bool check_builtin_no_params(char const *bname, Object const *expr) {
  return check_builtin_n_params(bname, expr, 0);
}

Object *memtotal_builtin(Object *expr) {
  size_t memtotal = get_total_memory_usage();
  return create_num_obj(memtotal);
}

Object *timeit_builtin(Object *expr) {
  if (!check_builtin_n_params("timeit", expr, 1)) return nil_obj;
  auto *expr_to_time = list_index(expr, 1);
  using std::chrono::duration;
  using std::chrono::duration_cast;
  using std::chrono::high_resolution_clock;
  using std::chrono::milliseconds;
  auto start_time = high_resolution_clock::now();
  // discard the result
  eval_expr(expr_to_time);
  auto end_time = high_resolution_clock::now();
  duration<double, std::milli> ms_double = end_time - start_time;
  auto running_time = ms_double.count();
  auto *rtime_s = new std::string(std::to_string(running_time));
  return create_str_obj(rtime_s);
}

Object *sleep_builtin(Object *expr) {
  if (!check_builtin_n_params("sleep", expr, 1)) return nil_obj;
  auto *ms_num_obj = list_index(expr, 1);
  auto ms = ms_num_obj->val.i_value;
  // sleep the execution thread
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return nil_obj;
}

static size_t call_stack_size = 0;
const size_t MAX_STACK_SIZE = 256;

Object *call_function(Object *fobj, Object *args_list) {
  if (call_stack_size > MAX_STACK_SIZE) {
    error_msg("Max call stack size reached");
    return nil_obj;
  }

  // Set arguments in the local scope
  auto *arglistl = fobj->val.f_value.funargs->val.l_value;
  auto *provided_arglistl = args_list->val.l_value;
  bool is_lambda = fobj->flags & OF_LAMBDA;
  // Lambda only have arguments int their arglist, while defuns
  // also have a function name as a first parameter. So we skip that
  // if needed.
  int starting_arg_idx = is_lambda ? 0 : 1;

  SymVars locals;
  auto set_symbol_local = [&](std::string &symname, Object *value) -> bool {
    // evaluate all arguments before calling
    // TODO: Maybe implement lazy evaluation for arguments with context binding?
    auto *evaluated = eval_expr(value);
    locals[symname] = evaluated;
    return true;
  };

  // Because calling function still means that the first element
  // of the list is either a (lambda ()) or a function name (callthis a b c)
  int provided_arg_offset = is_lambda ? 1 : 0;
  for (size_t arg_idx = starting_arg_idx; arg_idx < arglistl->size();
       ++arg_idx) {
    auto *arg = arglistl->at(arg_idx);
    auto *local_arg_name = arg->val.s_value;
    if (arg == dot_obj) {
      // we've reached the end of the usual argument list
      // now variadic arguments start
      // so skip this dot, parse the variadic list arg name, and exit
      if (arg_idx != (arglistl->size() - 2)) {
        // if the dot is not on the pre-last position, print out an error
        // message
        printf(
            "apply (.) operator in function definition incorrectly placed. "
            "It should be at the pre-last position, followed by a vararg "
            "list argument name\n");
        return nil_obj;
      }
      // read all arguments into a list and bind it to the local scope
      auto *varg = arglistl->at(arg_idx + 1);
      auto *varg_lobj = create_data_list_obj();
      for (auto provided_arg_idx = arg_idx;
           provided_arg_idx < provided_arglistl->size(); ++provided_arg_idx) {
        auto *provided_arg = provided_arglistl->at(provided_arg_idx);
        // user provided a dot argument, which means that a list containing
        // all the rest of variadic arguments must follow
        if (provided_arg == dot_obj) {
          // the dot must be on the pre-last position
          if (provided_arg_idx != provided_arglistl->size() - 2) {
            auto *fn = fun_name(fobj);
            error_msg(format(
                "Error while calling {}: dot notation on the caller side "
                "must be followed by a list argument containing the "
                "variadic expansion list\n",
                fn));
            return nil_obj;
          }
          // expand the rest
          auto *provided_variadic_list =
              eval_expr(provided_arglistl->at(provided_arg_idx + 1));
          if (provided_variadic_list->type != ObjType::List) {
            error_msg(
                "dot operator on caller side should always be "
                "followed by a list argument");
            return nil_obj;
          }
          for (size_t exp_idx = 0;
               exp_idx < list_length(provided_variadic_list); ++exp_idx) {
            auto *exp_item = list_index(provided_variadic_list, exp_idx);
            list_append_inplace(varg_lobj, exp_item);
          }
          // we're done with the argument list
          break;
        }
        list_append_inplace(varg_lobj, provided_arg);
      }
      set_symbol_local(*varg->val.s_value, varg_lobj);
      break;
    }
    if (arg_idx >= provided_arglistl->size()) {
      // Reached the end of the user-provided argument list, just
      // fill int nils for the remaining arguments
      set_symbol_local(*local_arg_name, nil_obj);
    } else {
      int provided_arg_idx = provided_arg_offset + arg_idx;
      auto *provided_arg = provided_arglistl->at(provided_arg_idx);
      set_symbol_local(*local_arg_name, provided_arg);
    }
  }
  auto *bodyl = fobj->val.f_value.funbody->val.l_value;
  int body_length = bodyl->size();
  // Starting from 1 because 1st index is function name
  int body_expr_idx = 2;
  Object *last_evaluated = nil_obj;
  ++call_stack_size;
  enter_scope_with(locals);
  while (body_expr_idx < body_length) {
    last_evaluated = eval_expr(bodyl->at(body_expr_idx));
    ++body_expr_idx;
  }
  exit_scope();
  --call_stack_size;
  return last_evaluated;
}

bool is_callable(Object *obj) { return obj->type == ObjType::Function; }

Object *eval_expr(Object *expr) {
  if (expr->flags & OF_EVALUATED) {
    return expr;
  }
  switch (expr->type) {
    case ObjType::Symbol: {
      // Look up value of the symbol in the symbol table
      auto *syms = expr->val.s_value;
      auto *res = get_symbol(*syms);
      bool present_in_symtable = res != nullptr;
      if (!present_in_symtable) {
        printf("Symbol not found: \"%s\"\n", syms->data());
        return nil_obj;
      }
      // If object is not yet evaluated
      if (!(res->flags & OF_EVALUATED)) {
        // Evaluate & save in the symbol table
        res = eval_expr(res);
        res->flags |= OF_EVALUATED;
        set_symbol(*syms, res);
      }
      return res;
    } break;
    case ObjType::List: {
      if (expr->flags & OF_LIST_LITERAL) {
        auto *items = list_members(expr);
        for (size_t i = 0; i < items->size(); ++i) {
          // do we need to evaluate here?
          (*items)[i] = eval_expr(items->at(i));
        }
        expr->flags |= OF_EVALUATED;
        return expr;
      }
      auto *l = expr->val.l_value;
      int elems_len = l->size();
      if (elems_len == 0) return expr;
      auto *op = l->at(0);
      auto *callable = eval_expr(op);
      if (!is_callable(callable)) {
        auto *s = obj_to_string_bare(callable);
        error_msg(format("\"{}\" is not callable", s->data()));
        delete s;
        return nil_obj;
      }
      bool is_builtin = callable->flags & OF_BUILTIN;
      if (is_builtin) {
        // Built-in function, no need to do much
        auto *bhandler = callable->val.bf_value.builtin_handler;
        return bhandler(expr);
      }
      // User-defined function
      return call_function(callable, expr);
    }
    default: {
      // For other types (string, number, nil) there is no need to evaluate them
      // as they are in their final form
      return expr;
    } break;
  }
}

bool load_file(path file_to_read) {
  auto s = read_whole_file_into_memory(file_to_read.c_str());
  IS.text = s.c_str();
  IS.file_name = file_to_read.c_str();
  IS.line = 0;
  IS.col = 0;
  if (IS.text == nullptr) {
    printf("Couldn't load file at %s, skipping\n", file_to_read.c_str());
    return false;
  }
  IS.text_len = strlen(IS.text);
  IS.text_pos = 0;
  while (IS.text_pos < IS.text_len) {
    auto *e = read_expr();
    eval_expr(e);
  }
  return true;
}

void init_interp() {
  // Initialize global symbol table
  IS.symtable = new SymTable();
  IS.symtable->prev = nullptr;
  nil_obj = create_nil_obj();
  true_obj = create_bool_obj(true);
  false_obj = create_bool_obj(false);
  dot_obj = create_final_sym_obj(".");
  else_obj = create_final_sym_obj(".");
  // initialize symtable with builtins
  set_symbol("nil", nil_obj);
  set_symbol("true", true_obj);
  set_symbol("false", false_obj);
  set_symbol("else", else_obj);
  create_builtin_function_and_save("+", (add_objects));
  create_builtin_function_and_save("-", (sub_objects));
  create_builtin_function_and_save("/", (div_objects_builtin));
  create_builtin_function_and_save("*", (mul_objects_builtin));
  create_builtin_function_and_save("**", (pow_objects_builtin));
  create_builtin_function_and_save("=", (equal_builtin));
  create_builtin_function_and_save(">", (gt_builtin));
  create_builtin_function_and_save("<", (lt_builtin));
  create_builtin_function_and_save("setq", (setq_builtin));
  create_builtin_function_and_save("print", (print_builtin));
  create_builtin_function_and_save("defun", (defun_builtin));
  create_builtin_function_and_save("lambda", (lambda_builtin));
  create_builtin_function_and_save("if", (if_builtin));
  create_builtin_function_and_save("car", (car_builtin));
  create_builtin_function_and_save("cdr", (cdr_builtin));
  create_builtin_function_and_save("cadr", (cadr_builtin));
  create_builtin_function_and_save("cond", (cond_builtin));
  create_builtin_function_and_save("memtotal", (memtotal_builtin));
  create_builtin_function_and_save("timeit", (timeit_builtin));
  create_builtin_function_and_save("sleep", (sleep_builtin));
  // Load the standard library
  path STDLIB_PATH = "./stdlib";
  load_file(STDLIB_PATH / path("basic.lisp"));
}

void run_interp() {
  std::string input;
  bool is_running = true;
  static std::string prompt = ">> ";
  IS.file_name = "interp";
  IS.line = 0;
  IS.col = 0;
  while (is_running) {
    std::cout << prompt;
    char c;
    while (std::cin.get(c) && c != '\n') {
      input += c;
    }
    if (input == ".exit") {
      is_running = false;
      continue;
    }
    IS.text = input.data();
    IS.text_pos = 0;
    IS.text_len = input.size();
    auto *e = read_expr();
    auto *res = eval_expr(e);
    auto *str_repr = obj_to_string_bare(res);
    std::cout << str_repr->data() << '\n';
    delete str_repr;
    input = "";
  }
}