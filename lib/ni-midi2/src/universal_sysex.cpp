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

#include <midi/universal_sysex.h>

//--------------------------------------------------------------------------

namespace midi::universal_sysex {

//-----------------------------------------------

identity_reply::identity_reply(
  manufacturer_t sysex_id, uint14_t family, uint14_t family_member, uint28_t revision, uint7_t device_id)
  : message(manufacturer::universal_non_realtime)
{
    const bool threeByteManufacturerID = (sysex_id < 0x10000);
    data.reserve(threeByteManufacturerID ? 14 : 12);

    data.push_back(device_id);
    data.push_back(0x06);
    data.push_back(subtype::identity_reply);

    if (threeByteManufacturerID)
    {
        data.push_back(0x00);
        data.push_back((sysex_id >> 8) & 0x7F);
        data.push_back(sysex_id & 0x7F);
    }
    else
    {
        data.push_back((sysex_id >> 16) & 0x7F);
    }

    add_uint14(family);
    add_uint14(family_member);
    add_uint28(revision);
}

//-----------------------------------------------

identity_reply::identity_reply(const device_identity& identity)
  : identity_reply(identity.manufacturer, identity.family, identity.model, identity.revision)
{
}

//--------------------------------------------------------------------------

} // namespace midi::universal_sysex

//--------------------------------------------------------------------------
