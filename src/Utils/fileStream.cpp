#include "fileStream.h"

using namespace std;

shared_ptr<utils::FileStream> utils::FileStream::open(string filename, ios::openmode mode)
{
    utils::FileStream* f = new utils::FileStream();
    f->file.open(filename, mode);
    return shared_ptr<utils::FileStream>(f);
}

void utils::FileStream::close()
{
    this->file.close();
}

unsigned long utils::FileStream::size()
{
    return this->file.tellg();
}

uint8_t utils::FileStream::read_byte()
{
    return (uint8_t)this->file.get();
}

void utils::FileStream::push_byte(uint8_t b)
{
    this->file << b;
}

unsigned long utils::FileStream::position()
{
    return this->file.tellp();
}

void utils::FileStream::set_position(unsigned long p)
{
    this->file.seekg(p, ios::beg);
}
