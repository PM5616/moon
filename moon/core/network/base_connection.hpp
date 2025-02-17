#pragma once
#include "config.hpp"
#include "asio.hpp"
#include "message.hpp"
#include "handler_alloc.hpp"
#include "const_buffers_holder.hpp"
#include "common/string.hpp"

namespace moon
{
    struct read_request
    {
        read_request()
            :delim(read_delim::CRLF)
            , size(0)
            , sessionid(0)
        {

        }

        read_request(read_delim d, size_t s, int32_t r)
            :delim(d)
            , size(s)
            , sessionid(r)
        {
        }

        read_request(const read_request&) = default;
        read_request& operator=(const read_request&) = default;

        read_delim delim;
        size_t size;
        int32_t sessionid;
    };

    class base_connection :public std::enable_shared_from_this<base_connection>
    {
    public:
        using socket_t = asio::ip::tcp::socket;

        using message_handler_t = std::function<void(const message_ptr_t&)>;

        template <typename... Args>
        explicit base_connection(uint32_t serviceid, uint8_t type, moon::socket* s, Args&&... args)
            : serviceid_(serviceid)
            , type_(type)
            , s_(s)
            , socket_(std::forward<Args>(args)...)
        {
        }

        base_connection(const base_connection&) = delete;

        base_connection& operator=(const base_connection&) = delete;

        virtual ~base_connection()
        {
        }

        virtual void start(bool accepted)
        {
            (void)accepted;
            //save remote addr
            asio::error_code ec;
            auto ep = socket_.remote_endpoint(ec);
            auto addr = ep.address();
            addr_ = addr.to_string(ec) + ":";
            addr_ += std::to_string(ep.port());

            recvtime_ = now();
        }

        virtual bool read(const read_request& ctx)
        {
            (void)ctx;
            return false;
        };

        virtual bool send(const buffer_ptr_t & data)
        {
            if (data == nullptr || data->size() == 0)
            {
                return false;
            }

            if (!socket_.is_open())
            {
                return false;
            }

            queue_.push_back(data);

            if (queue_.size() >= WARN_NET_SEND_QUEUE_SIZE)
            {
                CONSOLE_DEBUG(logger(), "network send queue too long. size:%zu", queue_.size());
                if (queue_.size() >= MAX_NET_SEND_QUEUE_SIZE)
                {
                    logic_error_ = network_logic_error::send_message_queue_size_max;
                    close();
                    return false;
                }
            }

            if (!sending_)
            {
                post_send();
            }
            return true;
        }

        void close(bool exit = false)
        {
            if (socket_.is_open())
            {
                asio::error_code ignore_ec;
                socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
                socket_.close(ignore_ec);
            }

            if (exit)
            {
                s_ = nullptr;
            }
        }

        socket_t& socket()
        {
            return socket_;
        }

        bool is_open() const
        {
            return socket_.is_open();
        }

        void fd(uint32_t fd)
        {
            fd_ = fd;
        }

        uint32_t fd() const
        {
            return fd_;
        }

        void timeout(time_t now)
        {
            if ((0 != timeout_) && (0 != recvtime_) && (now - recvtime_ > timeout_))
            {
                logic_error_ = network_logic_error::timeout;
                close();
            }
        }

        void set_no_delay()
        {
            asio::ip::tcp::no_delay option(true);
            asio::error_code ec;
            socket_.set_option(option, ec);
        }

        moon::log* logger() const
        {
            return log_;
        }

        void logger(moon::log* l)
        {
            log_ = l;
        }

        void settimeout(uint32_t v)
        {
            timeout_ = v;
        }

        static time_t now()
        {
            return std::time(nullptr);
        }
    protected:
        virtual void message_framing(const_buffers_holder& holder, buffer_ptr_t&& buf)
        {
            (void)holder;
            (void)buf;
        }

        void post_send()
        {
            holder_.clear();
            if (queue_.size() == 0)
                return;

            while ((queue_.size() != 0) && (holder_.size() < 50))
            {
                auto& msg = queue_.front();
                if (msg->has_flag(buffer_flag::framing))
                {
                    message_framing(holder_, std::move(msg));
                }
                else
                {
                    holder_.push_back(std::move(msg));
                }
                queue_.pop_front();
            }

            if (holder_.size() == 0)
                return;

            sending_ = true;
            asio::async_write(
                socket_,
                holder_.buffers(),
                make_custom_alloc_handler(wallocator_,
                    [this, self = shared_from_this()](const asio::error_code& e, std::size_t)
            {
                sending_ = false;

                if (!e)
                {
                    if (holder_.close())
                    {
                        close();
                    }
                    else
                    {
                        post_send();
                    }
                }
                else
                {
                    error(e, int(logic_error_));
                }
            }));
        }

        virtual void error(const asio::error_code& e, int lerrcode, const char* lerrmsg = nullptr)
        {
            //error
            {
                auto msg = message::create();
                std::string content;
                if (lerrcode)
                {
                    content = moon::format("{\"addr\":\"%s\",\"logic_errcode\":%d,\"errmsg\":\"%s\"}"
                        , addr_.data()
                        , lerrcode
                        , (lerrmsg == nullptr ? logic_errmsg(lerrcode) : lerrmsg)
                    );
                    msg->set_subtype(static_cast<uint8_t>(socket_data_type::socket_error));
                }
                else if (e && e != asio::error::eof)
                {
                    content = moon::format("{\"addr\":\"%s\",\"errcode\":%d,\"errmsg\":\"%s\"}"
                        , addr_.data()
                        , e.value()
                        , e.message().data());
                    msg->set_subtype(static_cast<uint8_t>(socket_data_type::socket_error));
                }

                msg->write_string(content);
                msg->set_sender(fd_);
                handle_message(std::move(msg));
            }

            //closed
            {
                auto msg = message::create();
                msg->write_string(addr_);
                msg->set_sender(fd_);
                msg->set_subtype(static_cast<uint8_t>(socket_data_type::socket_close));
                handle_message(std::move(msg));
            }
            s_ = nullptr;
        }

        template<typename Message>
        void handle_message(Message&& m)
        {
            if (nullptr != s_)
            {
                m->set_sender(fd_);
                if (m->type() == 0)
                {
                    m->set_type(type_);
                }
                s_->handle_message(serviceid_, std::forward<Message>(m));
            }
        }
    protected:
        bool sending_ = false;
        network_logic_error logic_error_ = network_logic_error::ok;
        uint32_t fd_ = 0;
        time_t recvtime_ = 0;
        uint32_t timeout_ = 0;
        moon::log* log_ = nullptr;
        uint32_t serviceid_;
        uint8_t type_;
        moon::socket* s_;
        socket_t socket_;
        std::string addr_;
        handler_allocator rallocator_;
        handler_allocator wallocator_;
        const_buffers_holder  holder_;
        std::deque<buffer_ptr_t> queue_;
    };
}