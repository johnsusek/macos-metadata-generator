#include "binaryReader.h"
#include "binaryStructures.h"
using namespace std;

const string binary::BinaryReader::read_string()
{
    string str = "";
    int8_t currentByte = this->read_byte();
    while (currentByte != '\0') {
        str.append(1, (char)currentByte);
        currentByte = this->read_byte();
    }
    return str;
}

binary::MetaFileOffset binary::BinaryReader::read_pointer()
{
    return this->read_number<binary::MetaFileOffset>(sizeof(MetaFileOffset));
}

binary::MetaArrayCount binary::BinaryReader::read_arrayCount()
{
    return this->read_number<binary::MetaArrayCount>(sizeof(MetaArrayCount));
}

vector<binary::MetaFileOffset> binary::BinaryReader::read_binaryArray()
{
    binary::MetaArrayCount arrayCount = this->read_arrayCount();
    vector<MetaFileOffset> elements;
    for (binary::MetaArrayCount i = 0; i < arrayCount; i++) {
        elements.push_back(this->read_pointer());
    }
    return elements;
}

int32_t binary::BinaryReader::read_int()
{
    return this->read_number<int32_t>(4);
}

int16_t binary::BinaryReader::read_short()
{
    return this->read_number<int16_t>(2);
}

uint8_t binary::BinaryReader::read_byte()
{
    return this->_stream->read_byte();
}
