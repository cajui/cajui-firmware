// SPDX-License-Identifier: Apache-2.0
#include "protocol_validation.h"
#include <cstring>

namespace cajui {
Result receive(const Binding& b, const Frame& frame, Journal& journal, Frame& ack) {
    ack = Frame{};
    Message m{};
    const auto decoded = open(b, frame, m);
    if (decoded != Result::Ok) return decoded;
    if (m.type != Type::Data) return Result::Invalid;
    Receipt old{};
    if (!journal.load(b, old)) return Result::StorageError;
    if (m.counter < old.counter) return Result::Replay;
    const bool duplicate = m.counter == old.counter;
    if (duplicate && !sameFrame(old.last, frame)) return Result::Conflict;
    if (!duplicate) {
        Receipt next{};
        next.counter = m.counter;
        next.last = frame;
        const auto saved = journal.commit(b, old.counter, next);
        if (saved != Result::Ok) return saved;
    }
    Message response{};
    response.type = Type::Ack;
    response.counter = m.counter;
    std::memcpy(response.dataTag.data(), frame.bytes.data() + frame.size - TagSize, TagSize);
    const auto sealed = seal(b, response, ack);
    if (sealed != Result::Ok) return sealed;
    return duplicate ? Result::Duplicate : Result::Ok;
}
Result Sender::begin(const Binding& b, const Data& data, CounterStore& store) {
    if (pending_.size && !delivered_) return Result::Conflict;
    if (!detail::usable(b)) return Result::Unauthorized;
    if (!detail::validData(data)) return Result::Invalid;
    uint64_t counter = 0;
    if (!store.reserve(b, counter) || !counter) return Result::StorageError;
    Message m{};
    m.counter = counter;
    m.data = data;
    Frame next{};
    const auto result = seal(b, m, next);
    if (result != Result::Ok) return result;
    binding_ = b;
    pending_ = next;
    counter_ = counter;
    attempts_ = 0;
    delivered_ = false;
    return Result::Ok;
}
void Sender::abandon() {
    pending_ = Frame{};
    counter_ = 0;
    attempts_ = 0;
    delivered_ = false;
}
const Frame* Sender::nextAttempt() {
    if (!pending_.size || delivered_ || attempts_ >= MaxAttempts) return nullptr;
    ++attempts_;
    return &pending_;
}
Result Sender::acknowledge(const Frame& ack) {
    if (!pending_.size || !attempts_) return Result::Invalid;
    Message m{};
    const auto result = open(binding_, ack, m);
    if (result != Result::Ok) return result;
    if (m.type != Type::Ack || m.counter != counter_ ||
        std::memcmp(m.dataTag.data(), pending_.bytes.data() + pending_.size - TagSize, TagSize) !=
            0)
        return Result::Invalid;
    delivered_ = true;
    return Result::Ok;
}
} // namespace cajui
