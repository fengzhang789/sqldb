# db

A small embedded database engine written in C++23. It has a disk-backed
B+tree storage layer with MVCC and atomic transactions, a catalog for table schemas
and secondary indexes, and a SQL-like query language that
compiles down to scans and index lookups.

The SQL database is built on top of a key value store "KV". KV is implemented with B+Trees and supports
transactions, commits and aborts. I plan on writing a post on my website on the implementation of the database
in further detail. I will link it in this readme once done.

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
- [Running Tests](#running-tests)
- [Project Layout](#project-layout)
- [Usage](#usage)
- [Future Work](#future-work)

## Features

- Disk-backed B+tree with 4KB pages, node splitting/merging, and a free list for
  page reuse
- MVCC: concurrent readers see a consistent snapshot while writers commit new
  versions, with a reader heap tracking the oldest active version
- Atomic transactions (`KVTX`/`DBTX`) with two-phase commit
- A catalog layer for table schemas, plus secondary indexes with range scans
- A small SQL dialect (`SELECT`, `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`, with
  `WHERE`/`FILTER`/`LIMIT`) parsed with Flex/Bison and executed against the storage
  engine

## Requirements

- CMake >= 3.16
- A C++23 compiler (developed against GCC 13)
- Flex and Bison

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

## Running Tests

```sh
cd build
ctest --output-on-failure
```

## Project Layout

```
include/   public headers, mirrors src/
src/       storage, catalog, access, ql (query language), encoding
tests/     GoogleTest suites, one per module
```

## Usage

`db.h` exposes the embeddable API:

```cpp
DB db(&kv);

DBTX tx;
db.begin(&tx);
tx.table_new(my_table_def, &err);
tx.insert("my_table", record, &err);
db.commit(&tx);
```

You need to create a KV instance based off a disk path.

Reads run against a snapshot via `DB::begin_read`/`DBReader`, and can run
concurrently with in-flight writers.

## Future Work

- [ ] Query optimizer — right now queries execute roughly as written, with no cost-based
      planning or predicate/index selection
- [ ] SPDK integration — experiment with kernel-bypass NVMe I/O in place of the
      current mmap/syscall-based page manager
- [ ] Rearchitect storage I/O so kernel-bypass (SPDK) and regular (syscall) modes are
      both supported and selectable via config
