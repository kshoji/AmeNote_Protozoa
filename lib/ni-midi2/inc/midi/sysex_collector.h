//
// Copyright (c) 2023 Native Instruments
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#pragma once

//--------------------------------------------------------------------------

#include <midi/sysex.h>
#include <midi/universal_packet.h>

#include <functional>
#include <utility>

//--------------------------------------------------------------------------

namespace midi {

//--------------------------------------------------------------------------

class sysex7_collector
{
  public:
    using callback = std::function<void(const sysex7&)>;

    explicit sysex7_collector(callback);

    void set_callback(callback);
    void set_max_sysex_data_size(size_t); //!< limit maximum size of accepted sysex data

    void feed(const universal_packet&);
    void reset();

  private:
    sysex7   m_sysex7;
    size_t   m_max_sysex_data_size{ 0 };
    status_t m_state{ data_status::sysex7_start };
    uint8_t  m_manufacturerIDBytesRead{ 0 };
    callback m_cb;
};

//--------------------------------------------------------------------------

class sysex8_collector
{
  public:
    using callback = std::function<void(const sysex8&, uint8_t stream_id)>;

    explicit sysex8_collector(callback);

    void set_callback(callback);
    void set_max_sysex_data_size(size_t); //!< limit maximum size of accepted sysex data

    void feed(const universal_packet&);
    void reset();

    uint8_t stream_id() const { return m_stream_id; }

  private:
    uint8_t       m_stream_id{ 0 };
    sysex8        m_sysex8;
    size_t        m_max_sysex_data_size{ 0 };
    packet_format m_state{ packet_format::start };
    enum { detect, one_byte, three_bytes, invalid, done } m_manufacturer_id_state = detect;
    callback m_cb;
};

//--------------------------------------------------------------------------

inline sysex7_collector::sysex7_collector(callback cb)
  : m_cb(std::move(cb))
{
}
inline void sysex7_collector::set_callback(callback cb)
{
    m_cb = std::move(cb);
}

//--------------------------------------------------------------------------

inline sysex8_collector::sysex8_collector(callback cb)
  : m_cb(std::move(cb))
{
}
inline void sysex8_collector::set_callback(callback cb)
{
    m_cb = std::move(cb);
}

//--------------------------------------------------------------------------

} // namespace midi

//--------------------------------------------------------------------------
