#include <unistd.h>
#include <cstring>

#include "Bus.h"
#include "AbstractSocket.h"

#include <fstream> // debug

namespace kickcat
{
    Bus::Bus(std::shared_ptr<AbstractSocket> socket)
        : socket_{socket}
        , frames_{}
    {
        frames_.emplace_back(PRIMARY_IF_MAC);
    }


    uint16_t Bus::getSlavesOnNetwork()
    {
        return static_cast<uint16_t>(slaves_.size());
    }


    uint16_t Bus::broadcastRead(uint16_t ADO, uint16_t data_size)
    {
        frames_[0].addDatagram(idx_, Command::BRD, createAddress(0, ADO), nullptr, data_size);
        Error err = frames_[0].writeThenRead(socket_);
        if (err)
        {
            err.what();
            return 0;
        }

        auto [header, _, wkc] = frames_[0].nextDatagram();
        ++idx_; // one more frame sent
        return wkc;
    }


    uint16_t Bus::broadcastWrite(uint16_t ADO, void const* data, uint16_t data_size)
    {
        frames_[0].addDatagram(idx_, Command::BWR, createAddress(0, ADO), data, data_size);
        Error err = frames_[0].writeThenRead(socket_);
        if (err)
        {
            err.what();
            return 0;
        }

        auto [header, _, wkc] = frames_[0].nextDatagram();
        ++idx_; // one more frame sent
        return wkc;
    }


    void Bus::addDatagram(enum Command command, uint32_t address, void const* data, uint16_t data_size)
    {
        auto* frame = &frames_[current_frame_];
        if ((frame->datagramCounter() == MAX_ETHERCAT_DATAGRAMS) or (frame->freeSpace() < data_size))
        {
            ++current_frame_;
            frame = &frames_[current_frame_];
        }
        frame->addDatagram(idx_, command, address, data, data_size);
    }


    Error Bus::processFrames()
    {
        current_frame_ = 0; // reset frame position

        for (auto& frame : frames_)
        {
            if (frame.datagramCounter() == 0)
            {
                break;
            }
            Error err = frame.writeThenRead(socket_);
            if (err)
            {
                return err;
            }
        }

        return ESUCCESS;
    }


    Error Bus::detectSlaves()
    {
        // we dont really care about the type, we just want a working counter to detect the number of slaves
        uint16_t wkc = broadcastRead(reg::TYPE, 1);
        if (wkc == 0)
        {
            return EERROR("No slaves on the bus");
        }

        slaves_.resize(wkc);

        // Allocate frames
        int32_t needed_frames = (wkc * 2 / MAX_ETHERCAT_DATAGRAMS + 1) * 2; // at one frame is required. We need to be able to send two datagram per slave in a row (mailboxes check)
        frames_.reserve(needed_frames);
        for (int i = 1; i < needed_frames; ++i)
        {
            frames_.emplace_back(PRIMARY_IF_MAC);
        }

        printf("-*-*-*- %d slave detected on the network -*-*-*-\n", slaves_.size());
        return ESUCCESS;
    }


    Error Bus::init()
    {
        Error err = detectSlaves();
        if (err) { return err; }

        err = resetSlaves();
        if (err) { return err; }

        // set addresses
        for (int i = 0; i < slaves_.size(); ++i)
        {
            slaves_[i].address = 0x1000 + i;
            addDatagram(Command::APRW, createAddress(0 - i, reg::STATION_ADDR), slaves_[i].address);
        }
        err = processFrames();
        if (err)
        {
            return err;
        }

        err = fetchEeprom();
        if (err)
        {
            return err;
        }

        err = configureMailboxes();
        if (err)
        {
            return err;
        }

        requestState(State::PRE_OP);
        usleep(10000); //TODO: wait for state

        checkMailboxes(slaves_);
        for (auto& slave : slaves_)
        {
            State state = getCurrentState(slave);
            printf("Slave %d state is %s - %x\n", slave.address, toString(state), state);
            printf("   - in ready %d | out ready %d\n", slave.mailbox.read_available, slave.mailbox.write_available);
        }


        err += EERROR("Not implemented");
        return err;
    }


    Error Bus::requestState(State request)
    {
        uint16_t param = request | State::ACK;
        uint16_t wkc = broadcastWrite(reg::AL_CONTROL, &param, sizeof(param));
        if (wkc != slaves_.size())
        {
            printf("aie %d %d\n", wkc, slaves_.size());
            return EERROR("failed to request state");
        }

        return ESUCCESS;
    }


    State Bus::getCurrentState(Slave const& slave)
    {
        frames_[0].addDatagram(0x55, Command::FPRD, createAddress(slave.address, reg::AL_STATUS), nullptr, 2);
        Error err = frames_[0].writeThenRead(socket_);
        if (err)
        {
            err.what();
            return State::INVALID;
        }

        auto [header, data, wkc] = frames_[0].nextDatagram();
        printf("--> data %x\n", data[0]);
        return State(data[0] & 0xF);
    }


    Error Bus::resetSlaves()
    {
        // buffer to reset them all
        uint8_t param[256];
        std::memset(param, 0, sizeof(param));

        // Set port to auto mode
        broadcastWrite(reg::ESC_DL_PORT,        param, 1);

        // Reset slaves registers
        broadcastWrite(reg::RX_ERROR,           param, 8);
        broadcastWrite(reg::FMMU,               param, 256);
        broadcastWrite(reg::SYNC_MANAGER,       param, 128);
        broadcastWrite(reg::DC_SYSTEM_TIME,     param, 8);
        broadcastWrite(reg::DC_SYNC_ACTIVATION, param, 1);

        uint16_t dc_param = 0x1000; // reset value
        broadcastWrite(reg::DC_SPEED_CNT_START, &dc_param, sizeof(dc_param));

        dc_param = 0x0c00;          // reset value
        broadcastWrite(reg::DC_TIME_FILTER, &dc_param, sizeof(dc_param));

        // Request INIT state
        Error err = requestState(State::INIT);
        if (err)
        {
            err += EERROR("");
            return err;
        }

        // eeprom to master
        broadcastWrite(reg::EEPROM_CONFIG, param, 2);

        return ESUCCESS;
    }


    Error Bus::configureMailboxes()
    {
        for (auto& slave : slaves_)
        {
            if (slave.supported_mailbox)
            {
                // 0 is mailbox out, 1 is mailbox in - cf. default EtherCAT configuration if slave support a mailbox
                // NOTE: mailbox out -> master to slave - mailbox in -> slave to master
                SyncManager SM[2];
                SM[0].start_address = slave.mailbox.recv_offset;
                SM[0].length        = slave.mailbox.recv_size;
                SM[0].control       = 0x26; // 1 buffer - write access - PDI IRQ ON
                SM[0].status        = 0x00; // RO register
                SM[0].activate      = 0x01; // Sync Manger enable
                SM[0].pdi_control   = 0x00; // RO register
                SM[1].start_address = slave.mailbox.send_offset;
                SM[1].length        = slave.mailbox.send_size;
                SM[1].control       = 0x22; // 1 buffer - read access - PDI IRQ ON
                SM[1].status        = 0x00; // RO register
                SM[1].activate      = 0x01; // Sync Manger enable
                SM[1].pdi_control   = 0x00; // RO register

                addDatagram(Command::FPRW, createAddress(slave.address, reg::SYNC_MANAGER), SM);
            }
        }

        return processFrames();
    }


    bool Bus::areEepromReady()
    {
        bool ready = false;
        for (int i = 0; (i < 10) and not ready; ++i)
        {
            usleep(200);
            for (auto& slave : slaves_)
            {
                addDatagram(Command::FPRD, createAddress(slave.address, reg::EEPROM_CONTROL), nullptr, 2);
            }
            Error err = processFrames();
            if (err)
            {
                err.what();
                return false;
            }

            DatagramHeader const* header;
            bool ready = true;
            do
            {
                auto [h, answer, wkc] = nextDatagram<uint16_t>();
                header = h;
                if (wkc != 1)
                {
                    Error err = EERROR("no answer!");
                    err.what();
                }
                if (*answer & 0x8000)
                {
                    ready = false;
                    for (auto& frame : frames_)
                    {
                        frame.clear();
                    }
                    break;
                }
            } while (header->multiple);

            if (ready)
            {
                return true;
            }
        }

        return false;
    }


    Error Bus::readEeprom(uint16_t address, std::function<void(Slave&, uint32_t word)> apply)
    {
        // eeprom request
        struct Request
        {
            uint16_t command;
            uint16_t addressLow;
            uint16_t addressHigh;
        } __attribute__((__packed__));

        Request req;

        // Request specific address
        req = { EepromCommand::READ, address, 0 };
        uint16_t wkc = broadcastWrite(reg::EEPROM_CONTROL, &req, sizeof(req));
        if (wkc != slaves_.size())
        {
            return EERROR("wrong slave number");
        }

        // wait for all eeprom to be ready
        if (not areEepromReady())
        {
            return EERROR("eeprom not ready - timeout");
        }

        // Read result
        for (auto& slave : slaves_)
        {
            addDatagram(Command::FPRD, createAddress(slave.address, reg::EEPROM_DATA), nullptr, 4);
        }
        Error err = processFrames();
        if (err)
        {
            return err;
        }

        // Extract result and store it
        for (auto& slave : slaves_)
        {
            auto [header, answer, wkc] = nextDatagram<uint32_t>();
            if (wkc != 1)
            {
                Error err = EERROR("no answer!");
                err.what();
            }
            apply(slave, *answer);
        }

        return ESUCCESS;
    }

    Error Bus::fetchEeprom()
    {
        // General slave info
        readEeprom(eeprom::VENDOR_ID,       [](Slave& s, uint32_t word) { s.vendor_id       = word; } );
        readEeprom(eeprom::PRODUCT_CODE,    [](Slave& s, uint32_t word) { s.product_code    = word; } );
        readEeprom(eeprom::REVISION_NUMBER, [](Slave& s, uint32_t word) { s.revision_number = word; } );
        readEeprom(eeprom::SERIAL_NUMBER,   [](Slave& s, uint32_t word) { s.serial_number   = word; } );

        // Mailbox info
        readEeprom(eeprom::STANDARD_MAILBOX + eeprom::RECV_MBO_OFFSET,
        [](Slave& s, uint32_t word) { s.mailbox.recv_offset = word; s.mailbox.recv_size = word >> 16; } );
        readEeprom(eeprom::STANDARD_MAILBOX + eeprom::SEND_MBO_OFFSET,
        [](Slave& s, uint32_t word) { s.mailbox.send_offset = word; s.mailbox.send_size = word >> 16; } );

        readEeprom(eeprom::MAILBOX_PROTOCOL, [](Slave& s, uint32_t word) { s.supported_mailbox = static_cast<MailboxProtocol>(word); });

        readEeprom(eeprom::EEPROM_SIZE,
        [](Slave& s, uint32_t word)
        {
            s.eeprom_size = (word & 0xFF) + 1; // 0 means 1024 bits
            s.eeprom_size *= 128;              // Kibit to bytes
            s.eeprom_version = word >> 16;
        });

        printSlavesInfo();

        return ESUCCESS;
    }


    void Bus::printSlavesInfo()
    {
        for (auto const& slave : slaves_)
        {
            printf("-*-*-*-*- slave 0x%04x -*-*-*-*-\n", slave.address);
            printf("Vendor ID:       0x%08x\n", slave.vendor_id);
            printf("Product code:    0x%08x\n", slave.product_code);
            printf("Revision number: 0x%08x\n", slave.revision_number);
            printf("Serial number:   0x%08x\n", slave.serial_number);
            printf("mailbox in:  size %d - offset 0x%04x\n", slave.mailbox.recv_size, slave.mailbox.recv_offset);
            printf("mailbox out: size %d - offset 0x%04x\n", slave.mailbox.send_size, slave.mailbox.send_offset);
            printf("supported mailbox protocol: 0x%02x\n", slave.supported_mailbox);
            printf("EEPROM: size: %d - version 0x%02x\n",  slave.eeprom_size, slave.eeprom_version);
            printf("\n");
        }
    }


    void Bus::checkMailboxes(std::vector<Slave>& slaves)
    {
        for (auto& slave : slaves)
        {
            addDatagram(Command::FPRD, createAddress(slave.address, reg::SYNC_MANAGER_0 + reg::SM_STATS), nullptr, 1);
            addDatagram(Command::FPRD, createAddress(slave.address, reg::SYNC_MANAGER_1 + reg::SM_STATS), nullptr, 1);
        }

        Error err = processFrames();
        if (err)
        {
            err.what();
            return;
        }

        for (auto& slave : slaves)
        {
            auto isFull = [this](bool stable_value)
            {
                auto [header, state, wkc] = nextDatagram<uint8_t>();
                if (wkc != 1)
                {
                    EERROR("error while reading mailboxes state").what();
                    return stable_value;
                }
                return ((*state & 0x08) == 0x08);
            };
            slave.mailbox.read_available  = isFull(false);
            slave.mailbox.write_available = isFull(true);
        }
    }
}