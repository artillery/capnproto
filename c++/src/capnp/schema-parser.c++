// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "schema-parser.h"
#include "message.h"
#include <capnp/compiler/compiler.h>
#include <capnp/compiler/lexer.capnp.h>
#include <capnp/compiler/lexer.h>
#include <capnp/compiler/grammar.capnp.h>
#include <capnp/compiler/parser.h>
#include <unordered_map>
#include <kj/mutex.h>
#include <kj/vector.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

namespace capnp {

namespace {

template <typename T>
size_t findLargestElementBefore(const kj::Vector<T>& vec, const T& key) {
  KJ_REQUIRE(vec.size() > 0 && vec[0] <= key);

  size_t lower = 0;
  size_t upper = vec.size();

  while (upper - lower > 1) {
    size_t mid = (lower + upper) / 2;
    if (vec[mid] > key) {
      upper = mid;
    } else {
      lower = mid;
    }
  }

  return lower;
}

}  // namespace

// =======================================================================================

class SchemaParser::ModuleImpl final: public compiler::Module {
public:
  ModuleImpl(const SchemaParser& parser, kj::Own<const SchemaFile>&& file)
      : parser(parser), file(kj::mv(file)) {}

  kj::StringPtr getSourceName() override {
    return file->getDisplayName();
  }

  Orphan<compiler::ParsedFile> loadContent(Orphanage orphanage) override {
    kj::Array<const char> content = file->readContent();

    lineBreaks.get([&](kj::SpaceFor<kj::Vector<uint>>& space) {
      auto vec = space.construct(content.size() / 40);
      vec->add(0);
      for (const char* pos = content.begin(); pos < content.end(); ++pos) {
        if (*pos == '\n') {
          vec->add(pos + 1 - content.begin());
        }
      }
      return vec;
    });

    MallocMessageBuilder lexedBuilder;
    auto statements = lexedBuilder.initRoot<compiler::LexedStatements>();
    compiler::lex(content, statements, *this);

    auto parsed = orphanage.newOrphan<compiler::ParsedFile>();
    compiler::parseFile(statements.getStatements(), parsed.get(), *this);
    return parsed;
  }

  kj::Maybe<Module&> importRelative(kj::StringPtr importPath) override {
    KJ_IF_MAYBE(importedFile, file->import(importPath)) {
      return parser.getModuleImpl(kj::mv(*importedFile));
    } else {
      return nullptr;
    }
  }

  void addError(uint32_t startByte, uint32_t endByte, kj::StringPtr message) override {
    auto& lines = lineBreaks.get(
        [](kj::SpaceFor<kj::Vector<uint>>& space) {
          KJ_FAIL_REQUIRE("Can't report errors until loadContent() is called.");
          return space.construct();
        });

    // TODO(someday):  This counts tabs as single characters.  Do we care?
    uint startLine = findLargestElementBefore(lines, startByte);
    uint startCol = startByte - lines[startLine];
    uint endLine = findLargestElementBefore(lines, endByte);
    uint endCol = endByte - lines[endLine];

    file->reportError(
        SchemaFile::SourcePos { startByte, startLine, startCol },
        SchemaFile::SourcePos { endByte, endLine, endCol },
        message);

    // We intentionally only set hadErrors true if reportError() didn't throw.
    parser.hadErrors = true;
  }

  bool hadErrors() override {
    return parser.hadErrors;
  }

private:
  const SchemaParser& parser;
  kj::Own<const SchemaFile> file;

  kj::Lazy<kj::Vector<uint>> lineBreaks;
  // Byte offsets of the first byte in each source line.  The first element is always zero.
  // Initialized the first time the module is loaded.
};

// =======================================================================================

namespace {

struct SchemaFileHash {
  inline bool operator()(const SchemaFile* f) const {
    return f->hashCode();
  }
};

struct SchemaFileEq {
  inline bool operator()(const SchemaFile* a, const SchemaFile* b) const {
    return *a == *b;
  }
};

}  // namespace

struct SchemaParser::Impl {
  typedef std::unordered_map<
      const SchemaFile*, kj::Own<ModuleImpl>, SchemaFileHash, SchemaFileEq> FileMap;
  kj::MutexGuarded<FileMap> fileMap;
  compiler::Compiler compiler;
};

SchemaParser::SchemaParser(): impl(kj::heap<Impl>()) {}
SchemaParser::~SchemaParser() noexcept(false) {}

ParsedSchema SchemaParser::parseDiskFile(
    kj::StringPtr displayName, kj::StringPtr diskPath,
    kj::ArrayPtr<const kj::StringPtr> importPath) const {
  return parseFile(SchemaFile::newDiskFile(displayName, diskPath, importPath));
}

ParsedSchema SchemaParser::parseFile(kj::Own<SchemaFile>&& file) const {
  KJ_DEFER(impl->compiler.clearWorkspace());
  uint64_t id = impl->compiler.add(getModuleImpl(kj::mv(file)));
  impl->compiler.eagerlyCompile(id,
      compiler::Compiler::NODE | compiler::Compiler::CHILDREN |
      compiler::Compiler::DEPENDENCIES | compiler::Compiler::DEPENDENCY_DEPENDENCIES);
  return ParsedSchema(impl->compiler.getLoader().get(id), *this);
}

SchemaParser::ModuleImpl& SchemaParser::getModuleImpl(kj::Own<SchemaFile>&& file) const {
  auto lock = impl->fileMap.lockExclusive();

  auto insertResult = lock->insert(std::make_pair(file.get(), kj::Own<ModuleImpl>()));
  if (insertResult.second) {
    // This is a newly-inserted entry.  Construct the ModuleImpl.
    insertResult.first->second = kj::heap<ModuleImpl>(*this, kj::mv(file));
  }
  return *insertResult.first->second;
}

kj::Maybe<ParsedSchema> ParsedSchema::findNested(kj::StringPtr name) const {
  return parser->impl->compiler.lookup(getProto().getId(), name).map(
      [this](uint64_t childId) {
        return ParsedSchema(parser->impl->compiler.getLoader().get(childId), *parser);
      });
}

ParsedSchema ParsedSchema::getNested(kj::StringPtr nestedName) const {
  KJ_IF_MAYBE(nested, findNested(nestedName)) {
    return *nested;
  } else {
    KJ_FAIL_REQUIRE("no such nested declaration", getProto().getDisplayName(), nestedName);
  }
}

// =======================================================================================

namespace {

class MmapDisposer: public kj::ArrayDisposer {
protected:
  void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                   size_t capacity, void (*destroyElement)(void*)) const {
    munmap(firstElement, elementSize * elementCount);
  }
};

constexpr MmapDisposer mmapDisposer = MmapDisposer();

static char* canonicalizePath(char* path) {
  // Taken from some old C code of mine.

  // Preconditions:
  // - path has already been determined to be relative, perhaps because the pointer actually points
  //   into the middle of some larger path string, in which case it must point to the character
  //   immediately after a '/'.

  // Invariants:
  // - src points to the beginning of a path component.
  // - dst points to the location where the path component should end up, if it is not special.
  // - src == path or src[-1] == '/'.
  // - dst == path or dst[-1] == '/'.

  char* src = path;
  char* dst = path;
  char* locked = dst;  // dst cannot backtrack past this
  char* partEnd;
  bool hasMore;

  for (;;) {
    while (*src == '/') {
      // Skip duplicate slash.
      ++src;
    }

    partEnd = strchr(src, '/');
    hasMore = partEnd != NULL;
    if (hasMore) {
      *partEnd = '\0';
    } else {
      partEnd = src + strlen(src);
    }

    if (strcmp(src, ".") == 0) {
      // Skip it.
    } else if (strcmp(src, "..") == 0) {
      if (dst > locked) {
        // Backtrack over last path component.
        --dst;
        while (dst > locked && dst[-1] != '/') --dst;
      } else {
        locked += 3;
        goto copy;
      }
    } else {
      // Copy if needed.
    copy:
      if (dst < src) {
        memmove(dst, src, partEnd - src);
        dst += partEnd - src;
      } else {
        dst = partEnd;
      }
      *dst++ = '/';
    }

    if (hasMore) {
      src = partEnd + 1;
    } else {
      // Oops, we have to remove the trailing '/'.
      if (dst == path) {
        // Oops, there is no trailing '/'.  We have to return ".".
        strcpy(path, ".");
        return path + 1;
      } else {
        // Remove the trailing '/'.  Note that this means that opening the file will work even
        // if it is not a directory, where normally it should fail on non-directories when a
        // trailing '/' is present.  If this is a problem, we need to add some sort of special
        // handling for this case where we stat() it separately to check if it is a directory,
        // because Ekam findInput will not accept a trailing '/'.
        --dst;
        *dst = '\0';
        return dst;
      }
    }
  }
}

kj::String canonicalizePath(kj::StringPtr path) {
  KJ_STACK_ARRAY(char, result, path.size() + 1, 128, 512);
  strcpy(result.begin(), path.begin());

  char* start = path.startsWith("/") ? result.begin() + 1 : result.begin();
  char* end = canonicalizePath(start);
  return kj::heapString(result.slice(0, end - result.begin()));
}

kj::String relativePath(kj::StringPtr base, kj::StringPtr add) {
  if (add.size() > 0 && add[0] == '/') {
    return kj::heapString(add);
  }

  const char* pos = base.end();
  while (pos > base.begin() && pos[-1] != '/') {
    --pos;
  }

  return kj::str(base.slice(0, pos - base.begin()), add);
}

kj::String joinPath(kj::StringPtr base, kj::StringPtr add) {
  KJ_REQUIRE(!add.startsWith("/"));

  return kj::str(base, '/', add);
}

}  // namespace

const SchemaFile::DiskFileReader SchemaFile::DiskFileReader::instance =
    SchemaFile::DiskFileReader();

bool SchemaFile::DiskFileReader::exists(kj::StringPtr path) const {
  return access(path.cStr(), F_OK) == 0;
}

kj::Array<const char> SchemaFile::DiskFileReader::read(kj::StringPtr path) const {
  int fd;
  // We already established that the file exists, so this should not fail.
  KJ_SYSCALL(fd = open(path.cStr(), O_RDONLY), path);
  kj::AutoCloseFd closer(fd);

  struct stat stats;
  KJ_SYSCALL(fstat(fd, &stats));

  if (S_ISREG(stats.st_mode)) {
    if (stats.st_size == 0) {
      // mmap()ing zero bytes will fail.
      return nullptr;
    }

    // Regular file.  Just mmap() it.
    const void* mapping = mmap(NULL, stats.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      KJ_FAIL_SYSCALL("mmap", errno, path);
    }

    return kj::Array<const char>(
        reinterpret_cast<const char*>(mapping), stats.st_size, mmapDisposer);
  } else {
    // This could be a stream of some sort, like a pipe.  Fall back to read().
    // TODO(cleanup):  This does a lot of copies.  Not sure I care.
    kj::Vector<char> data(8192);

    char buffer[4096];
    for (;;) {
      ssize_t n;
      KJ_SYSCALL(n = ::read(fd, buffer, sizeof(buffer)));
      if (n == 0) break;
      data.addAll(buffer, buffer + n);
    }

    return data.releaseAsArray();
  }
}

// -------------------------------------------------------------------

class SchemaFile::DiskSchemaFile final: public SchemaFile {
public:
  DiskSchemaFile(const FileReader& fileReader, kj::String displayName,
                 kj::String diskPath, kj::ArrayPtr<const kj::StringPtr> importPath)
      : fileReader(fileReader),
        displayName(kj::mv(displayName)),
        diskPath(kj::mv(diskPath)),
        importPath(importPath) {}

  kj::StringPtr getDisplayName() const override {
    return displayName;
  }

  kj::Array<const char> readContent() const override {
    return fileReader.read(diskPath);
  }

  kj::Maybe<kj::Own<SchemaFile>> import(kj::StringPtr path) const override {
    if (path.startsWith("/")) {
      for (auto candidate: importPath) {
        kj::String newDiskPath = canonicalizePath(joinPath(candidate, path.slice(1)));
        if (fileReader.exists(newDiskPath)) {
          return kj::implicitCast<kj::Own<SchemaFile>>(kj::heap<DiskSchemaFile>(
              fileReader, canonicalizePath(path.slice(1)),
              kj::mv(newDiskPath), importPath));
        }
      }
      return nullptr;
    } else {
      kj::String newDiskPath = canonicalizePath(relativePath(diskPath, path));
      if (fileReader.exists(newDiskPath)) {
        return kj::implicitCast<kj::Own<SchemaFile>>(kj::heap<DiskSchemaFile>(
            fileReader, canonicalizePath(relativePath(displayName, path)),
            kj::mv(newDiskPath), importPath));
      } else {
        return nullptr;
      }
    }
  }

  bool operator==(const SchemaFile& other) const override {
    return diskPath == kj::downcast<const DiskSchemaFile>(other).diskPath;
  }
  bool operator!=(const SchemaFile& other) const override {
    return diskPath != kj::downcast<const DiskSchemaFile>(other).diskPath;
  }
  size_t hashCode() const override {
    // djb hash with xor
    // TODO(someday):  Add hashing library to KJ.
    size_t result = 5381;
    for (char c: diskPath) {
      result = (result * 33) ^ c;
    }
    return result;
  }

  void reportError(SourcePos start, SourcePos end, kj::StringPtr message) const override {
    kj::getExceptionCallback().onRecoverableException(kj::Exception(
        kj::Exception::Nature::LOCAL_BUG, kj::Exception::Durability::PERMANENT,
        kj::heapString(diskPath), start.line, kj::heapString(message)));
  }

private:
  const FileReader& fileReader;
  kj::String displayName;
  kj::String diskPath;
  kj::ArrayPtr<const kj::StringPtr> importPath;
};

kj::Own<SchemaFile> SchemaFile::newDiskFile(
    kj::StringPtr displayName, kj::StringPtr diskPath,
    kj::ArrayPtr<const kj::StringPtr> importPath,
    const FileReader& fileReader) {
  return kj::heap<DiskSchemaFile>(fileReader, canonicalizePath(displayName),
                                  canonicalizePath(diskPath), importPath);
}

}  // namespace capnp
