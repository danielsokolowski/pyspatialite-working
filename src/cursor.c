/* cursor.c - the cursor type
 *
 * Copyright (C) 2004-2005 Gerhard H�ring <gh@ghaering.de>
 *
 * This file is part of pysqlite.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "cursor.h"
#include "module.h"
#include "util.h"
#include "microprotocols.h"
#include "prepare_protocol.h"

/* used to decide wether to call PyInt_FromLong or PyLong_FromLongLong */
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647

PyObject* cursor_iternext(Cursor *self);

static StatementType detect_statement_type(char* statement)
{
    char buf[20];
    char* src;
    char* dst;

    src = statement;
    /* skip over whitepace */
    while (*src == '\r' || *src == '\n' || *src == ' ' || *src == '\t') {
        src++;
    }

    if (*src == 0)
        return STATEMENT_INVALID;

    dst = buf;
    *dst = 0;
    while (isalpha(*src) && dst - buf < sizeof(buf) - 2) {
        *dst++ = tolower(*src++);
    }

    *dst = 0;

    if (!strcmp(buf, "select")) {
        return STATEMENT_SELECT;
    } else if (!strcmp(buf, "insert")) {
        return STATEMENT_INSERT;
    } else if (!strcmp(buf, "update")) {
        return STATEMENT_UPDATE;
    } else if (!strcmp(buf, "delete")) {
        return STATEMENT_DELETE;
    } else if (!strcmp(buf, "replace")) {
        return STATEMENT_REPLACE;
    } else {
        return STATEMENT_OTHER;
    }
}

int cursor_init(Cursor* self, PyObject* args, PyObject* kwargs)
{
    Connection* connection;

    self->statement = NULL;

    if (!PyArg_ParseTuple(args, "O!", &ConnectionType, &connection))
    {
        return -1; 
    }

    Py_INCREF(connection);
    self->connection = connection;
    self->statement = NULL;
    self->step_rc = UNKNOWN;

    self->row_cast_map = PyList_New(0);

    Py_INCREF(Py_None);
    self->description = Py_None;

    Py_INCREF(Py_None);
    self->lastrowid= Py_None;

    self->arraysize = 1;

    self->rowcount = PyInt_FromLong(-1L);

    Py_INCREF(Py_None);
    self->row_factory = Py_None;

    if (!check_thread(self->connection)) {
        return -1;
    }

    return 0;
}

void cursor_dealloc(Cursor* self)
{
    int rc;

    /* Finalize the statement if the user has not closed the cursor */
    if (self->statement) {
        Py_BEGIN_ALLOW_THREADS
        rc = sqlite3_finalize(self->statement);
        Py_END_ALLOW_THREADS
    }

    Py_XDECREF(self->connection);
    Py_XDECREF(self->row_cast_map);
    Py_XDECREF(self->description);
    Py_XDECREF(self->lastrowid);
    Py_XDECREF(self->rowcount);
    Py_XDECREF(self->row_factory);

    self->ob_type->tp_free((PyObject*)self);
}

void build_row_cast_map(Cursor* self)
{
    int i;
    const unsigned char* type_start = (const unsigned char*)-1;
    const unsigned char* pos;

    const unsigned char* colname;
    const unsigned char* decltype;
    PyObject* py_decltype;
    PyObject* converter;
    PyObject* key;

    if (!self->connection->detect_types) {
        return;
    }

    Py_DECREF(self->row_cast_map);
    self->row_cast_map = PyList_New(0);

    for (i = 0; i < sqlite3_column_count(self->statement); i++) {
        converter = NULL;

        if (self->connection->detect_types | PARSE_COLNAMES) {
            colname = sqlite3_column_name(self->statement, i);

            for (pos = colname; *pos != 0; pos++) {
                if (*pos == '[') {
                    type_start = pos + 1;
                } else if (*pos == ']' && type_start != (const unsigned char*)-1) {
                    key = PyString_FromStringAndSize(type_start, pos - type_start);
                    converter = PyDict_GetItem(converters, key);
                    Py_DECREF(key);
                    break;
                }

            }
        }

        if (!converter && self->connection->detect_types | PARSE_DECLTYPES) {
            decltype = sqlite3_column_decltype(self->statement, i);
            if (decltype) {
                for (pos = decltype;;pos++) {
                    if (*pos == ' ' || *pos == 0) {
                        py_decltype = PyString_FromStringAndSize(decltype, pos - decltype);
                        break;
                    }
                }

                converter = PyDict_GetItem(converters, py_decltype);
                Py_DECREF(py_decltype);
            }
        }

        if (converter) {
            PyList_Append(self->row_cast_map, converter);
        } else {
            PyList_Append(self->row_cast_map, Py_None);
        }
    }
}

int _bind_parameter(Cursor* self, int pos, PyObject* parameter)
{
    int rc = SQLITE_OK;
    long longval;
#ifdef HAVE_LONG_LONG
    PY_LONG_LONG longlongval;
#endif
    const char* buffer;
    char* string;
    int buflen;
    PyObject* stringval;

    if (parameter == Py_None) {
        rc = sqlite3_bind_null(self->statement, pos);
    } else if (PyInt_Check(parameter)) {
        longval = PyInt_AsLong(parameter);
        rc = sqlite3_bind_int64(self->statement, pos, (sqlite_int64)longval);
#ifdef HAVE_LONG_LONG
    } else if (PyLong_Check(parameter)) {
        longlongval = PyLong_AsLongLong(parameter);
        /* in the overflow error case, longlongval is -1, and an exception is set */
        rc = sqlite3_bind_int64(self->statement, pos, (sqlite_int64)longlongval);
#endif
    } else if (PyFloat_Check(parameter)) {
        rc = sqlite3_bind_double(self->statement, pos, PyFloat_AsDouble(parameter));
    } else if (PyBuffer_Check(parameter)) {
        if (PyObject_AsCharBuffer(parameter, &buffer, &buflen) == 0) {
            rc = sqlite3_bind_blob(self->statement, pos, buffer, buflen, SQLITE_TRANSIENT);
        } else {
            PyErr_SetString(PyExc_ValueError, "could not convert BLOB to buffer");
            rc = -1;
        }
    } else if PyString_Check(parameter) {
        string = PyString_AsString(parameter);
        rc = sqlite3_bind_text(self->statement, pos, string, -1, SQLITE_TRANSIENT);
    } else if PyUnicode_Check(parameter) {
        stringval = PyUnicode_AsUTF8String(parameter);
        string = PyString_AsString(stringval);
        rc = sqlite3_bind_text(self->statement, pos, string, -1, SQLITE_TRANSIENT);
        Py_DECREF(stringval);
    } else {
        rc = -1;
    }

    return rc;
}

PyObject* _build_column_name(const unsigned char* colname)
{
    const unsigned char* pos;

    for (pos = colname;; pos++) {
        if (*pos == 0 || *pos == ' ') {
            return PyString_FromStringAndSize(colname, pos - colname);
        }
    }
}

typedef enum {
    LINECOMMENT_1,
    IN_LINECOMMENT,
    COMMENTSTART_1,
    IN_COMMENT,
    COMMENTEND_1,
    NORMAL
} parse_remaining_sql_state;

/*
 * Checks if there is anything left in an SQL string after SQLite compiled it.
 * This is used to check if somebody tried to execute more than one SQL command
 * with one execute()/executemany() command, which the DB-API and we don't
 * allow.
 *
 * Returns 1 if there is more left than should be. 0 if ok.
 */
int check_remaining_sql(const char* tail)
{
    const char* pos = tail;

    parse_remaining_sql_state state = NORMAL;

    for (;;) {
        switch (*pos) {
            case 0:
                return 0;
            case '-':
                if (state == NORMAL) {
                    state  = LINECOMMENT_1;
                } else if (state == LINECOMMENT_1) {
                    state = IN_LINECOMMENT;
                }
                break;
            case ' ':
            case '\t':
                break;
            case '\n':
            case 13:
                if (state == IN_LINECOMMENT) {
                    state = NORMAL;
                }
                break;
            case '/':
                if (state == NORMAL) {
                    state = COMMENTSTART_1;
                } else if (state == COMMENTEND_1) {
                    state = NORMAL;
                } else if (state == COMMENTSTART_1) {
                    return 1;
                }
                break;
            case '*':
                if (state == NORMAL) {
                    return 1;
                } else if (state == LINECOMMENT_1) {
                    return 1;
                } else if (state == COMMENTSTART_1) {
                    state = IN_COMMENT;
                } else if (state == IN_COMMENT) {
                    state = COMMENTEND_1;
                }
                break;
            default:
                if (state == COMMENTEND_1) {
                    state = IN_COMMENT;
                } else if (state == IN_LINECOMMENT) {
                } else if (state == IN_COMMENT) {
                } else {
                    return 1;
                }
        }

        pos++;
    }

    return 0;
}

PyObject* _query_execute(Cursor* self, int multiple, PyObject* args)
{
    PyObject* operation;
    PyObject* operation_bytestr = NULL;
    char* operation_cstr;
    PyObject* parameters_list = NULL;
    PyObject* parameters_iter = NULL;
    PyObject* parameters = NULL;
    int num_params;
    const char* tail;
    int i;
    int rc;
    PyObject* func_args;
    PyObject* result;
    int numcols;
    int statement_type;
    PyObject* descriptor;
    PyObject* current_param;
    PyObject* adapted;
    PyObject* second_argument = NULL;
    int num_params_needed;
    const char* binding_name;
    long rowcount = 0;

    if (!check_thread(self->connection) || !check_connection(self->connection)) {
        return NULL;
    }

    if (multiple) {
        /* executemany() */
        if (!PyArg_ParseTuple(args, "OO", &operation, &second_argument)) {
            return NULL; 
        }

        if (!PyString_Check(operation) && !PyUnicode_Check(operation)) {
            PyErr_SetString(PyExc_ValueError, "operation parameter must be str or unicode");
            return NULL;
        }

        if (PyIter_Check(second_argument)) {
            /* iterator */
            Py_INCREF(second_argument);
            parameters_iter = second_argument;
        } else {
            /* sequence */
            parameters_iter = PyObject_GetIter(second_argument);
            if (PyErr_Occurred())
            {
                return NULL;
            }
        }
    } else {
        /* execute() */
        if (!PyArg_ParseTuple(args, "O|O", &operation, &second_argument)) {
            return NULL; 
        }

        if (!PyString_Check(operation) && !PyUnicode_Check(operation)) {
            PyErr_SetString(PyExc_ValueError, "operation parameter must be str or unicode");
            return NULL;
        }

        parameters_list = PyList_New(0);
        if (!parameters_list) {
            return NULL;
        }

        if (second_argument == NULL) {
            second_argument = PyTuple_New(0);
        } else {
            Py_INCREF(second_argument);
        }
        PyList_Append(parameters_list, second_argument);
        Py_DECREF(second_argument);

        parameters_iter = PyObject_GetIter(parameters_list);
    }

    if (self->statement != NULL) {
        /* There is an active statement */
        Py_BEGIN_ALLOW_THREADS
        rc = sqlite3_finalize(self->statement);
        Py_END_ALLOW_THREADS
    }

    if (PyString_Check(operation)) {
        operation_cstr = PyString_AsString(operation);
    } else {
        operation_bytestr = PyUnicode_AsUTF8String(operation);
        if (!operation_bytestr) {
            goto error;
        }

        operation_cstr = PyString_AsString(operation_bytestr);
    }

    /* reset description and rowcount */
    Py_DECREF(self->description);
    Py_INCREF(Py_None);
    self->description = Py_None;

    Py_DECREF(self->rowcount);
    self->rowcount = PyInt_FromLong(-1L);

    statement_type = detect_statement_type(operation_cstr);
    if (self->connection->begin_statement) {
        switch (statement_type) {
            case STATEMENT_UPDATE:
            case STATEMENT_DELETE:
            case STATEMENT_INSERT:
            case STATEMENT_REPLACE:
                if (!self->connection->inTransaction) {
                    result = _connection_begin(self->connection);
                    if (!result) {
                        goto error;
                    }
                    Py_DECREF(result);
                }
                break;
            case STATEMENT_OTHER:
                /* it's a DDL statement or something similar
                   - we better COMMIT first so it works for all cases */
                if (self->connection->inTransaction) {
                    func_args = PyTuple_New(0);
                    result = connection_commit(self->connection, func_args);
                    Py_DECREF(func_args);
                    if (!result) {
                        goto error;
                    }
                    Py_DECREF(result);
                }
                break;
            case STATEMENT_SELECT:
                if (multiple) {
                    PyErr_SetString(ProgrammingError,
                                "You can only execute SELECT statements in executemany().");
                    goto error;
                }
        }
    }

    Py_BEGIN_ALLOW_THREADS
    rc = sqlite3_prepare(self->connection->db,
                         operation_cstr,
                         0,
                         &self->statement,
                         &tail);
    Py_END_ALLOW_THREADS
    if (rc != SQLITE_OK) {
        _seterror(self->connection->db);
        goto error;
    }

    if (check_remaining_sql(tail)) {
        PyErr_SetString(Warning, "You can only execute one statement at a time.");
        goto error;
    }

    Py_BEGIN_ALLOW_THREADS
    num_params_needed = sqlite3_bind_parameter_count(self->statement);
    Py_END_ALLOW_THREADS
    while (1) {
        parameters = PyIter_Next(parameters_iter);
        if (!parameters) {
            break;
        }

        if (PyDict_Check(parameters)) {
            /* parameters passed as dictionary */
            for (i = 1; i <= num_params_needed; i++) {
                Py_BEGIN_ALLOW_THREADS
                binding_name = sqlite3_bind_parameter_name(self->statement, i);
                Py_END_ALLOW_THREADS
                if (!binding_name) {
                    PyErr_Format(ProgrammingError, "Binding %d has no name, but you supplied a dictionary (which has only names).", i);
                    goto error;
                }

                binding_name++; /* skip first char (the colon) */
                current_param = PyDict_GetItemString(parameters, binding_name);
                if (!current_param) {
                    PyErr_Format(ProgrammingError, "You did not supply a value for binding %d.", i);
                    goto error;
                }

                Py_INCREF(current_param);
                adapted = microprotocols_adapt(current_param, (PyObject*)&SQLitePrepareProtocolType, NULL);
                if (adapted) {
                    Py_DECREF(current_param);
                } else {
                    PyErr_Clear();
                    adapted = current_param;
                }

                rc = _bind_parameter(self, i, adapted);
                Py_DECREF(adapted);

                if (rc != SQLITE_OK) {
                    PyErr_Format(InterfaceError, "Error binding parameter :%s - probably unsupported type.", binding_name);
                    goto error;
               }
            }
        } else {
            /* parameters passed as sequence */
            num_params = PySequence_Length(parameters);
            if (num_params != num_params_needed) {
                PyErr_Format(ProgrammingError, "Incorrect number of bindings supplied. The current statement uses %d, and there are %d supplied.",
                             num_params_needed, num_params);
                goto error;
            }
            for (i = 0; i < num_params; i++) {
                current_param = PySequence_GetItem(parameters, i);
                adapted = microprotocols_adapt(current_param, (PyObject*)&SQLitePrepareProtocolType, NULL);

                if (adapted) {
                    Py_DECREF(current_param);
                } else {
                    PyErr_Clear();
                    adapted = current_param;
                }

                rc = _bind_parameter(self, i + 1, adapted);
                Py_DECREF(adapted);

                if (rc != SQLITE_OK) {
                    PyErr_Format(InterfaceError, "Error binding parameter %d - probably unsupported type.", i);
                    goto error;
                }

            }
        }

        build_row_cast_map(self);

        self->step_rc = _sqlite_step_with_busyhandler(self->statement, self->connection);
        if (self->step_rc != SQLITE_DONE && self->step_rc != SQLITE_ROW) {
            _seterror(self->connection->db);
            goto error;
        }

        if (self->step_rc == SQLITE_ROW) {
            if (multiple) {
                PyErr_SetString(ProgrammingError, "executemany() can only execute DML statements.");
                goto error;
            }

            Py_BEGIN_ALLOW_THREADS
            numcols = sqlite3_data_count(self->statement);
            Py_END_ALLOW_THREADS

            if (self->description == Py_None) {
                Py_DECREF(self->description);
                self->description = PyTuple_New(numcols);
                for (i = 0; i < numcols; i++) {
                    descriptor = PyTuple_New(7);
                    PyTuple_SetItem(descriptor, 0, _build_column_name(sqlite3_column_name(self->statement, i)));
                    Py_INCREF(Py_None); PyTuple_SetItem(descriptor, 1, Py_None);
                    Py_INCREF(Py_None); PyTuple_SetItem(descriptor, 2, Py_None);
                    Py_INCREF(Py_None); PyTuple_SetItem(descriptor, 3, Py_None);
                    Py_INCREF(Py_None); PyTuple_SetItem(descriptor, 4, Py_None);
                    Py_INCREF(Py_None); PyTuple_SetItem(descriptor, 5, Py_None);
                    Py_INCREF(Py_None); PyTuple_SetItem(descriptor, 6, Py_None);
                    PyTuple_SetItem(self->description, i, descriptor);
                }
            }
        }

        switch (statement_type) {
            case STATEMENT_UPDATE:
            case STATEMENT_DELETE:
            case STATEMENT_INSERT:
            case STATEMENT_REPLACE:
                Py_BEGIN_ALLOW_THREADS
                rowcount += (long)sqlite3_changes(self->connection->db);
                Py_END_ALLOW_THREADS
                Py_DECREF(self->rowcount);
                self->rowcount = PyInt_FromLong(rowcount);
        }

        Py_DECREF(self->lastrowid);
        if (statement_type == STATEMENT_INSERT) {
            Py_BEGIN_ALLOW_THREADS
            self->lastrowid = PyInt_FromLong((long)sqlite3_last_insert_rowid(self->connection->db));
            Py_END_ALLOW_THREADS
        } else {
            Py_INCREF(Py_None);
            self->lastrowid = Py_None;
        }

        if (multiple) {
            Py_BEGIN_ALLOW_THREADS
            rc = sqlite3_reset(self->statement);
            Py_END_ALLOW_THREADS
        }
        Py_XDECREF(parameters);
    }

error:
    Py_XDECREF(operation_bytestr);
    Py_XDECREF(parameters);
    Py_DECREF(parameters_iter);
    Py_XDECREF(parameters_list);

    if (PyErr_Occurred()) {
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

PyObject* cursor_execute(Cursor* self, PyObject* args)
{
    return _query_execute(self, 0, args);
}

PyObject* cursor_executemany(Cursor* self, PyObject* args)
{
    return _query_execute(self, 1, args);
}

PyObject* cursor_executescript(Cursor* self, PyObject* args)
{
    PyObject* script_obj;
    PyObject* script_str = NULL;
    const char* script_cstr;
    sqlite3_stmt* statement;
    int rc;
    PyObject* func_args;
    PyObject* result;

    if (!PyArg_ParseTuple(args, "O", &script_obj)) {
        return NULL; 
    }

    if (!check_thread(self->connection) || !check_connection(self->connection)) {
        return NULL;
    }

    if (PyString_Check(script_obj)) {
        script_cstr = PyString_AsString(script_obj);
    } else if (PyUnicode_Check(script_obj)) {
        script_str = PyUnicode_AsUTF8String(script_obj);
        if (!script_obj) {
            return NULL;
        }

        script_cstr = PyString_AsString(script_str);
    } else {
        PyErr_SetString(PyExc_ValueError, "script argument must be unicode or string.");
        return NULL;
    }

    /* commit first */
    func_args = PyTuple_New(0);
    result = connection_commit(self->connection, func_args);
    Py_DECREF(func_args);
    if (!result) {
        goto error;
    }
    Py_DECREF(result);

    while (1) {
        if (!sqlite3_complete(script_cstr)) {
            break;
        }

        rc = sqlite3_prepare(self->connection->db,
                             script_cstr,
                             0,
                             &statement,
                             &script_cstr);
        if (rc != SQLITE_OK) {
            _seterror(self->connection->db);
            goto error;
        }

        /* execute statement, and ignore results of SELECT statements */
        rc = SQLITE_ROW;
        while (rc == SQLITE_ROW) {
            rc = _sqlite_step_with_busyhandler(statement, self->connection);
        }

        if (rc != SQLITE_DONE) {
            (void)sqlite3_finalize(statement);
            _seterror(self->connection->db);
            goto error;
        }

        rc = sqlite3_finalize(statement);
        if (rc != SQLITE_OK) {
            _seterror(self->connection->db);
            goto error;
        }
    }

error:
    Py_XDECREF(script_str);

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* cursor_getiter(Cursor *self)
{
    Py_INCREF(self);
    return (PyObject*)self;
}

PyObject* cursor_iternext(Cursor *self)
{
    int i, numcols;
    PyObject* row;
    PyObject* converted_row;
    PyObject* item = NULL;
    int coltype;
    long long intval;
    PyObject* converter;
    PyObject* converted;
    int nbytes;
    PyObject* buffer;
    void* raw_buffer;
    const char* val_str;

    if (!check_thread(self->connection) || !check_connection(self->connection)) {
        return NULL;
    }

    if (self->statement == NULL) {
        PyErr_SetString(ProgrammingError, "no compiled statement - you need to execute() before you can fetch data");
        return NULL;
    }

    /* TODO: handling of step_rc here. it would be nicer if the hack with
             UNKNOWN were not necessary
    */
    if (self->step_rc == UNKNOWN) {
        self->step_rc = _sqlite_step_with_busyhandler(self->statement, self->connection);
    }

    if (self->step_rc == SQLITE_DONE) {
        return NULL;
    } else if (self->step_rc != SQLITE_ROW) {
        _seterror(self->connection->db);
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    numcols = sqlite3_data_count(self->statement);
    Py_END_ALLOW_THREADS

    row = PyTuple_New(numcols);

    for (i = 0; i < numcols; i++) {
        if (self->connection->detect_types) {
            converter = PyList_GetItem(self->row_cast_map, i);
            if (!converter) {
                converter = Py_None;
            }
        } else {
            converter = Py_None;
        }

        if (converter != Py_None) {
            val_str = sqlite3_column_text(self->statement, i);
            if (!val_str) {
                Py_INCREF(Py_None);
                converted = Py_None;
            } else {
                item = PyString_FromString(val_str);
                converted = PyObject_CallFunction(converter, "O", item);
                if (!converted) {
                    /* TODO: have a way to log these errors */
                    Py_INCREF(Py_None);
                    converted = Py_None;
                    PyErr_Clear();
                }
                Py_DECREF(item);
            }
        } else {
            Py_BEGIN_ALLOW_THREADS
            coltype = sqlite3_column_type(self->statement, i);
            Py_END_ALLOW_THREADS
            if (coltype == SQLITE_NULL) {
                Py_INCREF(Py_None);
                converted = Py_None;
            } else if (coltype == SQLITE_INTEGER) {
                intval = sqlite3_column_int64(self->statement, i);
                if (intval < INT32_MIN || intval > INT32_MAX) {
                    converted = PyLong_FromLongLong(intval);
                } else {
                    converted = PyInt_FromLong((long)intval);
                }
            } else if (coltype == SQLITE_FLOAT) {
                converted = PyFloat_FromDouble(sqlite3_column_double(self->statement, i));
            } else if (coltype == SQLITE_TEXT) {
                val_str = sqlite3_column_text(self->statement, i);
                converted = PyUnicode_DecodeUTF8(val_str, strlen(val_str), NULL);
            } else {
                /* coltype == SQLITE_BLOB */
                nbytes = sqlite3_column_bytes(self->statement, i);
                buffer = PyBuffer_New(nbytes);
                if (!buffer) {
                    break;
                }
                if (PyObject_AsWriteBuffer(buffer, &raw_buffer, &nbytes)) {
                    break;
                }
                memcpy(raw_buffer, sqlite3_column_blob(self->statement, i), nbytes);
                converted = buffer;
            }
        }

        PyTuple_SetItem(row, i, converted);
    }

    if (PyErr_Occurred()) {
        self->step_rc = UNKNOWN;
        return NULL;
    } else {
        self->step_rc = UNKNOWN;

        if (self->row_factory != Py_None) {
            converted_row = PyObject_CallFunction(self->row_factory, "OO", self, row);
            Py_DECREF(row);
        } else {
            converted_row = row;
        }

        return converted_row;
    }
}

PyObject* cursor_fetchone(Cursor* self, PyObject* args)
{
    PyObject* row;

    row = cursor_iternext(self);
    if (!row && !PyErr_Occurred()) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    return row;
}

PyObject* cursor_fetchmany(Cursor* self, PyObject* args)
{
    PyObject* row;
    PyObject* list;
    int maxrows = self->arraysize;
    int counter = 0;

    if (!PyArg_ParseTuple(args, "|i", &maxrows)) {
        return NULL; 
    }

    list = PyList_New(0);

    /* just make sure we enter the loop */
    row = (PyObject*)1;

    while (row) {
        row = cursor_iternext(self);
        if (row) {
            PyList_Append(list, row);
            Py_DECREF(row);
        } else {
            break;
        }

        if (++counter == maxrows) {
            break;
        }
    }

    if (PyErr_Occurred()) {
        Py_DECREF(list);
        return NULL;
    } else {
        return list;
    }
}

PyObject* cursor_fetchall(Cursor* self, PyObject* args)
{
    PyObject* row;
    PyObject* list;

    list = PyList_New(0);

    /* just make sure we enter the loop */
    row = (PyObject*)1;

    while (row) {
        row = cursor_iternext(self);
        if (row) {
            PyList_Append(list, row);
            Py_DECREF(row);
        }
    }

    if (PyErr_Occurred()) {
        Py_DECREF(list);
        return NULL;
    } else {
        return list;
    }
}

PyObject* pysqlite_noop(Connection* self, PyObject* args)
{
    /* don't care, return None */
    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* cursor_close(Cursor* self, PyObject* args)
{
    int rc;

    if (!check_thread(self->connection) || !check_connection(self->connection)) {
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    rc = sqlite3_finalize(self->statement);
    Py_END_ALLOW_THREADS

    Py_INCREF(Py_None);
    return Py_None;
}

static PyMethodDef cursor_methods[] = {
    {"execute", (PyCFunction)cursor_execute, METH_VARARGS,
        PyDoc_STR("Executes a SQL statement.")},
    {"executemany", (PyCFunction)cursor_executemany, METH_VARARGS,
        PyDoc_STR("Repeatedly executes a SQL statement.")},
    {"executescript", (PyCFunction)cursor_executescript, METH_VARARGS,
        PyDoc_STR("Executes a multiple SQL statements at once. Non-standard.")},
    {"fetchone", (PyCFunction)cursor_fetchone, METH_NOARGS,
        PyDoc_STR("Fetches several rows from the resultset.")},
    {"fetchmany", (PyCFunction)cursor_fetchmany, METH_VARARGS,
        PyDoc_STR("Fetches all rows from the resultset.")},
    {"fetchall", (PyCFunction)cursor_fetchall, METH_NOARGS,
        PyDoc_STR("Fetches one row from the resultset.")},
    {"close", (PyCFunction)cursor_close, METH_NOARGS,
        PyDoc_STR("Closes the cursor.")},
    {"setinputsizes", (PyCFunction)pysqlite_noop, METH_VARARGS,
        PyDoc_STR("Required by DB-API. Does nothing in pysqlite.")},
    {"setoutputsize", (PyCFunction)pysqlite_noop, METH_VARARGS,
        PyDoc_STR("Required by DB-API. Does nothing in pysqlite.")},
    {NULL, NULL}
};

static struct PyMemberDef cursor_members[] =
{
    {"connection", T_OBJECT, offsetof(Cursor, connection), RO},
    {"description", T_OBJECT, offsetof(Cursor, description), RO},
    {"arraysize", T_INT, offsetof(Cursor, arraysize), 0},
    {"lastrowid", T_OBJECT, offsetof(Cursor, lastrowid), RO},
    {"rowcount", T_OBJECT, offsetof(Cursor, rowcount), RO},
    {"row_factory", T_OBJECT, offsetof(Cursor, row_factory), 0},
    {NULL}
};

PyTypeObject CursorType = {
        PyObject_HEAD_INIT(NULL)
        0,                                              /* ob_size */
        "pysqlite2.dbapi2.Cursor",                      /* tp_name */
        sizeof(Cursor),                                 /* tp_basicsize */
        0,                                              /* tp_itemsize */
        (destructor)cursor_dealloc,                     /* tp_dealloc */
        0,                                              /* tp_print */
        0,                                              /* tp_getattr */
        0,                                              /* tp_setattr */
        0,                                              /* tp_compare */
        0,                                              /* tp_repr */
        0,                                              /* tp_as_number */
        0,                                              /* tp_as_sequence */
        0,                                              /* tp_as_mapping */
        0,                                              /* tp_hash */
        0,                                              /* tp_call */
        0,                                              /* tp_str */
        0,                                              /* tp_getattro */
        0,                                              /* tp_setattro */
        0,                                              /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT|Py_TPFLAGS_HAVE_ITER|Py_TPFLAGS_BASETYPE, /* tp_flags */
        0,                                              /* tp_doc */
        0,                                              /* tp_traverse */
        0,                                              /* tp_clear */
        0,                                              /* tp_richcompare */
        0,                                              /* tp_weaklistoffset */
        (getiterfunc)cursor_getiter,                    /* tp_iter */
        (iternextfunc)cursor_iternext,                  /* tp_iternext */
        cursor_methods,                                 /* tp_methods */
        cursor_members,                                 /* tp_members */
        0,                                              /* tp_getset */
        0,                                              /* tp_base */
        0,                                              /* tp_dict */
        0,                                              /* tp_descr_get */
        0,                                              /* tp_descr_set */
        0,                                              /* tp_dictoffset */
        (initproc)cursor_init,                          /* tp_init */
        0,                                              /* tp_alloc */
        0,                                              /* tp_new */
        0                                               /* tp_free */
};
