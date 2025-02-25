/**
 * @file tests/synctests.cpp
 * @brief Mega SDK test file
 *
 * (c) 2018 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

// Many of these tests are still being worked on.
// The file uses some C++17 mainly for the very convenient std::filesystem library, though the main SDK must still build with C++11 (and prior)


#include "test.h"
#include "stdfs.h"
#include <mega.h>
#include "gtest/gtest.h"
#include <stdio.h>
#include <map>
#include <future>
//#include <mega/tsthooks.h>
#include <fstream>
#include <atomic>
#include <random>

#include <megaapi_impl.h>

#define DEFAULTWAIT std::chrono::seconds(20)

using namespace ::mega;
using namespace ::std;


template<typename T>
using shared_promise = std::shared_ptr<promise<T>>;

using PromiseBoolSP   = shared_promise<bool>;
using PromiseHandleSP = shared_promise<handle>;
using PromiseStringSP = shared_promise<string>;

PromiseBoolSP newPromiseBoolSP()
{
    return PromiseBoolSP(new promise<bool>());
}



#ifdef ENABLE_SYNC

namespace {

bool suppressfiles = false;

typedef ::mega::byte byte;

// Creates a temporary directory in the current path
fs::path makeTmpDir(const int maxTries = 1000)
{
    const auto cwd = fs::current_path();
    std::random_device dev;
    std::mt19937 prng{dev()};
    std::uniform_int_distribution<uint64_t> rand{0};
    fs::path path;
    for (int i = 0;; ++i)
    {
        std::ostringstream os;
        os << std::hex << rand(prng);
        path = cwd / os.str();
        if (fs::create_directory(path))
        {
            break;
        }
        if (i == maxTries)
        {
            throw std::runtime_error{"Couldn't create tmp dir"};
        }
    }
    return path;
}

// Copies a file while maintaining the write time.
void copyFile(const fs::path& source, const fs::path& target)
{
    assert(fs::is_regular_file(source));
    const auto tmpDir = makeTmpDir();
    const auto tmpFile = tmpDir / "copied_file";
    fs::copy_file(source, tmpFile);
    fs::last_write_time(tmpFile, fs::last_write_time(source));
    fs::rename(tmpFile, target);
    fs::remove(tmpDir);
}

string leafname(const string& p)
{
    auto n = p.find_last_of("/");
    return n == string::npos ? p : p.substr(n+1);
}

string parentpath(const string& p)
{
    auto n = p.find_last_of("/");
    return n == string::npos ? "" : p.substr(0, n-1);
}

void WaitMillisec(unsigned n)
{
#ifdef _WIN32
    if (n > 1000)
    {
        for (int i = 0; i < 10; ++i)
        {
            // better for debugging, with breakpoints, pauses, etc
            Sleep(n/10);
        }
    }
    else
    {
        Sleep(n);
    }
#else
    usleep(n * 1000);
#endif
}

bool createFile(const fs::path &path, const void *data, const size_t data_length)
{
#if (__cplusplus >= 201700L)
    ofstream ostream(path, ios::binary);
#else
    ofstream ostream(path.u8string(), ios::binary);
#endif

    ostream.write(reinterpret_cast<const char *>(data), data_length);

    return ostream.good();
}

bool createDataFile(const fs::path &path, const std::string &data)
{
    return createFile(path, data.data(), data.size());
}

bool createDataFile(const fs::path& path, const std::string& data, std::chrono::seconds delta)
{
    if (!createDataFile(path, data)) return false;

    std::error_code result;
    auto current = fs::last_write_time(path, result);

    if (result) return false;

    fs::last_write_time(path, current + delta, result);

    return !result;
}

std::string randomData(const std::size_t length)
{
    std::vector<uint8_t> data(length);

    std::generate_n(data.begin(), data.size(), [](){ return (uint8_t)std::rand(); });

    return std::string((const char*)data.data(), data.size());
}

struct Model
{
    // records what we think the tree should look like after sync so we can confirm it

    struct ModelNode
    {
        enum nodetype { file, folder };
        nodetype type = folder;
        string mCloudName;
        string mFsName;
        string name;
        string content;
        vector<unique_ptr<ModelNode>> kids;
        ModelNode* parent = nullptr;
        bool changed = false;

        ModelNode() = default;

        ModelNode(const ModelNode& other)
          : type(other.type)
          , mCloudName()
          , mFsName()
          , name(other.name)
          , content(other.content)
          , kids()
          , parent()
          , changed(other.changed)
        {
            for (auto& child : other.kids)
            {
                addkid(child->clone());
            }
        }

        ModelNode& fsName(const string& name)
        {
            return mFsName = name, *this;
        }

        const string& fsName() const
        {
            return mFsName.empty() ? name : mFsName;
        }

        ModelNode& cloudName(const string& name)
        {
            return mCloudName = name, *this;
        }

        const string& cloudName() const
        {
            return mCloudName.empty() ? name : mCloudName;
        }

        void generate(const fs::path& path, bool force)
        {
            const fs::path ourPath = path / fsName();

            if (type == file)
            {
                if (changed || force)
                {
                    ASSERT_TRUE(createDataFile(ourPath, content));
                    changed = false;
                }
            }
            else
            {
                fs::create_directory(ourPath);

                for (auto& child : kids)
                {
                    child->generate(ourPath, force);
                }
            }
        }

        string path()
        {
            string s;
            for (auto p = this; p; p = p->parent)
                s = "/" + p->name + s;
            return s;
        }

        ModelNode* addkid()
        {
            return addkid(::mega::make_unique<ModelNode>());
        }

        ModelNode* addkid(unique_ptr<ModelNode>&& p)
        {
            p->parent = this;
            kids.emplace_back(move(p));

            return kids.back().get();
        }

        bool typematchesnodetype(nodetype_t nodetype) const
        {
            switch (type)
            {
            case file: return nodetype == FILENODE;
            case folder: return nodetype == FOLDERNODE;
            }
            return false;
        }

        void print(string prefix="")
        {
            out() << prefix << name;
            prefix.append(name).append("/");
            for (const auto &in: kids)
            {
                in->print(prefix);
            }
        }

        std::unique_ptr<ModelNode> clone()
        {
            return ::mega::make_unique<ModelNode>(*this);
        }
    };

    Model()
      : root(makeModelSubfolder("root"))
    {
    }

    Model(const Model& other)
      : root(other.root->clone())
    {
    }

    Model& operator=(const Model& rhs)
    {
        Model temp(rhs);

        swap(temp);

        return *this;
    }

    ModelNode* addfile(const string& path, const string& content)
    {
        auto* node = addnode(path, ModelNode::file);

        node->content = content;
        node->changed = true;

        return node;
    }

    ModelNode* addfile(const string& path)
    {
        return addfile(path, path);
    }

    ModelNode* addfolder(const string& path)
    {
        return addnode(path, ModelNode::folder);
    }

    ModelNode* addnode(const string& path, ModelNode::nodetype type)
    {
        ModelNode* child;
        ModelNode* node = root.get();
        string name;
        size_t current = 0;
        size_t end = path.size();

        while (current < end)
        {
            size_t delimiter = path.find('/', current);

            if (delimiter == path.npos)
            {
                break;
            }

            name = path.substr(current, delimiter - current);

            if (!(child = childnodebyname(node, name)))
            {
                child = node->addkid();

                child->name = name;
                child->type = ModelNode::folder;
            }

            assert(child->type == ModelNode::folder);

            current = delimiter + 1;
            node = child;
        }

        assert(current < end);

        name = path.substr(current);

        if (!(child = childnodebyname(node, name)))
        {
            child = node->addkid();

            child->name = name;
            child->type = type;
        }

        assert(child->type == type);

        return child;
    }

    ModelNode* copynode(const string& src, const string& dst)
    {
        const ModelNode* source = findnode(src);
        ModelNode* destination = addnode(dst, source->type);

        destination->content = source->content;
        destination->kids.clear();

        for (auto& child : source->kids)
        {
            destination->addkid(child->clone());
        }

        return destination;
    }

    unique_ptr<ModelNode> makeModelSubfolder(const string& utf8Name)
    {
        unique_ptr<ModelNode> n(new ModelNode);
        n->name = utf8Name;
        return n;
    }

    unique_ptr<ModelNode> makeModelSubfile(const string& utf8Name, string content = {})
    {
        unique_ptr<ModelNode> n(new ModelNode);
        n->name = utf8Name;
        n->type = ModelNode::file;
        n->content = content.empty() ? utf8Name : std::move(content);
        return n;
    }

    unique_ptr<ModelNode> buildModelSubdirs(const string& prefix, int n, int recurselevel, int filesperdir)
    {
        if (suppressfiles) filesperdir = 0;

        unique_ptr<ModelNode> nn = makeModelSubfolder(prefix);

        for (int i = 0; i < filesperdir; ++i)
        {
            nn->addkid(makeModelSubfile("file" + to_string(i) + "_" + prefix));
        }

        if (recurselevel > 0)
        {
            for (int i = 0; i < n; ++i)
            {
                unique_ptr<ModelNode> sn = buildModelSubdirs(prefix + "_" + to_string(i), n, recurselevel - 1, filesperdir);
                sn->parent = nn.get();
                nn->addkid(move(sn));
            }
        }
        return nn;
    }

    ModelNode* childnodebyname(ModelNode* n, const std::string& s)
    {
        for (auto& m : n->kids)
        {
            if (m->name == s)
            {
                return m.get();
            }
        }
        return nullptr;
    }

    ModelNode* findnode(string path, ModelNode* startnode = nullptr)
    {
        ModelNode* n = startnode ? startnode : root.get();
        while (n && !path.empty())
        {
            auto pos = path.find("/");
            n = childnodebyname(n, path.substr(0, pos));
            path.erase(0, pos == string::npos ? path.size() : pos + 1);
        }
        return n;
    }

    unique_ptr<ModelNode> removenode(const string& path)
    {
        ModelNode* n = findnode(path);
        if (n && n->parent)
        {
            unique_ptr<ModelNode> extracted;
            ModelNode* parent = n->parent;
            auto newend = std::remove_if(parent->kids.begin(), parent->kids.end(), [&extracted, n](unique_ptr<ModelNode>& v) { if (v.get() == n) return extracted = move(v), true; else return false; });
            parent->kids.erase(newend, parent->kids.end());
            return extracted;
        }
        return nullptr;
    }

    bool movenode(const string& sourcepath, const string& destpath)
    {
        ModelNode* source = findnode(sourcepath);
        ModelNode* dest = findnode(destpath);
        if (source && source && source->parent && dest)
        {
            auto replaced_node = removenode(destpath + "/" + source->name);

            unique_ptr<ModelNode> n;
            ModelNode* parent = source->parent;
            auto newend = std::remove_if(parent->kids.begin(), parent->kids.end(), [&n, source](unique_ptr<ModelNode>& v) { if (v.get() == source) return n = move(v), true; else return false; });
            parent->kids.erase(newend, parent->kids.end());
            if (n)
            {
                dest->addkid(move(n));
                return true;
            }
        }
        return false;
    }

    bool movetosynctrash(const string& path, const string& syncrootpath)
    {
        ModelNode* syncroot;
        if (!(syncroot = findnode(syncrootpath)))
        {
            return false;
        }

        ModelNode* trash;
        if (!(trash = childnodebyname(syncroot, DEBRISFOLDER)))
        {
            auto uniqueptr = makeModelSubfolder(DEBRISFOLDER);
            trash = uniqueptr.get();
            syncroot->addkid(move(uniqueptr));
        }

        char today[50];
        auto rawtime = time(NULL);
        strftime(today, sizeof today, "%F", localtime(&rawtime));

        ModelNode* dayfolder;
        if (!(dayfolder = findnode(today, trash)))
        {
            auto uniqueptr = makeModelSubfolder(today);
            dayfolder = uniqueptr.get();
            trash->addkid(move(uniqueptr));
        }

        if (auto uniqueptr = removenode(path))
        {
            dayfolder->addkid(move(uniqueptr));
            return true;
        }
        return false;
    }

    void ensureLocalDebrisTmpLock(const string& syncrootpath)
    {
        // if we've downloaded a file then it's put in debris/tmp initially, and there is a lock file
        if (ModelNode* syncroot = findnode(syncrootpath))
        {
            ModelNode* trash;
            if (!(trash = childnodebyname(syncroot, DEBRISFOLDER)))
            {
                auto uniqueptr = makeModelSubfolder(DEBRISFOLDER);
                trash = uniqueptr.get();
                syncroot->addkid(move(uniqueptr));
            }

            ModelNode* tmpfolder;
            if (!(tmpfolder = findnode("tmp", trash)))
            {
                auto uniqueptr = makeModelSubfolder("tmp");
                tmpfolder = uniqueptr.get();
                trash->addkid(move(uniqueptr));
            }

            ModelNode* lockfile;
            if (!(lockfile = findnode("lock", tmpfolder)))
            {
                tmpfolder->addkid(makeModelSubfile("lock"));
            }
        }
    }

    bool removesynctrash(const string& syncrootpath, const string& subpath = "")
    {
        if (subpath.empty())
        {
            return removenode(syncrootpath + "/" + DEBRISFOLDER).get();
        }
        else
        {
            char today[50];
            auto rawtime = time(NULL);
            strftime(today, sizeof today, "%F", localtime(&rawtime));

            return removenode(syncrootpath + "/" + DEBRISFOLDER + "/" + today + "/" + subpath).get();
        }
    }

    void emulate_rename(std::string nodepath, std::string newname)
    {
        auto node = findnode(nodepath);
        ASSERT_TRUE(!!node);
        if (node) node->name = newname;
    }

    void emulate_move(std::string nodepath, std::string newparentpath)
    {
        auto removed = removenode(newparentpath + "/" + leafname(nodepath));

        ASSERT_TRUE(movenode(nodepath, newparentpath));
    }

    void emulate_copy(std::string nodepath, std::string newparentpath)
    {
        auto node = findnode(nodepath);
        auto newparent = findnode(newparentpath);
        ASSERT_TRUE(!!node);
        ASSERT_TRUE(!!newparent);
        newparent->addkid(node->clone());
    }

    void emulate_rename_copy(std::string nodepath, std::string newparentpath, std::string newname)
    {
        auto node = findnode(nodepath);
        auto newparent = findnode(newparentpath);
        ASSERT_TRUE(!!node);
        ASSERT_TRUE(!!newparent);
        auto newnode = node->clone();
        newnode->name = newname;
        newparent->addkid(std::move(newnode));
    }

    void emulate_delete(std::string nodepath)
    {
        auto removed = removenode(nodepath);
       // ASSERT_TRUE(!!removed);
    }

    void generate(const fs::path& path, bool force = false)
    {
        fs::create_directories(path);

        for (auto& child : root->kids)
        {
            child->generate(path, force);
        }
    }

    void swap(Model& other)
    {
        using std::swap;

        swap(root, other.root);
    }

    unique_ptr<ModelNode> root;
};


bool waitonresults(future<bool>* r1 = nullptr, future<bool>* r2 = nullptr, future<bool>* r3 = nullptr, future<bool>* r4 = nullptr)
{
    if (r1) r1->wait();
    if (r2) r2->wait();
    if (r3) r3->wait();
    if (r4) r4->wait();
    return (!r1 || r1->get()) && (!r2 || r2->get()) && (!r3 || r3->get()) && (!r4 || r4->get());
}

atomic<int> next_request_tag{ 1 << 30 };

struct StandardClient : public MegaApp
{
    WAIT_CLASS waiter;
#ifdef GFX_CLASS
    GFX_CLASS gfx;
#endif

    string client_dbaccess_path;
    std::unique_ptr<HttpIO> httpio;
    std::unique_ptr<FileSystemAccess> fsaccess;
    std::recursive_mutex clientMutex;
    MegaClient client;
    std::atomic<bool> clientthreadexit{false};
    bool fatalerror = false;
    string clientname;
    std::function<void()> nextfunctionMC;
    std::function<void()> nextfunctionSC;
    std::condition_variable functionDone;
    std::mutex functionDoneMutex;
    std::string salt;
    std::set<fs::path> localFSFilesThatMayDiffer;

    fs::path fsBasePath;

    handle basefolderhandle = UNDEF;

    enum resultprocenum { PRELOGIN, LOGIN, FETCHNODES, PUTNODES, UNLINK, MOVENODE, CATCHUP, SETATTR,
                          COMPLETION };  // use COMPLETION when we use a completion function, rather than trying to match tags on callbacks

    struct ResultProc
    {
        StandardClient& client;
        ResultProc(StandardClient& c) : client(c) {}

        struct id_callback
        {
            int request_tag = 0;
            handle h = UNDEF;
            std::function<bool(error)> f;
            id_callback(std::function<bool(error)> cf, int tag, handle ch) : request_tag(tag), h(ch), f(cf) {}
        };

        recursive_mutex mtx;  // recursive because sometimes we need to set up new operations during a completion callback
        map<resultprocenum, deque<id_callback>> m;

        void prepresult(resultprocenum rpe, int tag, std::function<void()>&& requestfunc, std::function<bool(error)>&& f, handle h = UNDEF)
        {
            if (rpe != COMPLETION)
            {
                lock_guard<recursive_mutex> g(mtx);
                auto& entry = m[rpe];
                entry.emplace_back(move(f), tag, h);
            }

            std::lock_guard<std::recursive_mutex> lg(client.clientMutex);

            assert(tag > 0);
            int oldtag = client.client.reqtag;
            client.client.reqtag = tag;
            requestfunc();
            client.client.reqtag = oldtag;

            client.client.waiter->notify();
        }

        void processresult(resultprocenum rpe, error e, handle h = UNDEF)
        {
            int tag = client.client.restag;
            if (tag == 0 && rpe != CATCHUP)
            {
                //out() << "received notification of SDK initiated operation " << rpe << " tag " << tag; // too many of those to output
                return;
            }

            if (tag < (2 << 30))
            {
                out() << "ignoring callback from SDK internal sync operation " << rpe << " tag " << tag;
                return;
            }

            lock_guard<recursive_mutex> g(mtx);
            auto& entry = m[rpe];

            if (rpe == CATCHUP)
            {
                while (!entry.empty())
                {
                    entry.front().f(e);
                    entry.pop_front();
                }
                return;
            }

            if (entry.empty())
            {
                //out() << client.client.clientname
                //      << "received notification of operation type " << rpe << " completion but we don't have a record of it.  tag: " << tag;
                return;
            }

            if (tag != entry.front().request_tag)
            {
                out() << client.client.clientname
                      << "tag mismatch for operation completion of " << rpe << " tag " << tag << ", we expected " << entry.front().request_tag;
                return;
            }

            if (entry.front().f(e))
            {
                entry.pop_front();
            }
        }
    } resultproc;

    // thread as last member so everything else is initialised before we start it
    std::thread clientthread;

    string ensureDir(const fs::path& p)
    {
        fs::create_directories(p);

        string result = p.u8string();

        if (result.back() != fs::path::preferred_separator)
        {
            result += fs::path::preferred_separator;
        }

        return result;
    }

    StandardClient(const fs::path& basepath, const string& name)
        : client_dbaccess_path(ensureDir(basepath / name))
        , httpio(new HTTPIO_CLASS)
        , fsaccess(new FSACCESS_CLASS(makeFsAccess_<FSACCESS_CLASS>()))
        , client(this,
                 &waiter,
                 httpio.get(),
                 fsaccess.get(),
#ifdef DBACCESS_CLASS
                 new DBACCESS_CLASS(LocalPath::fromPath(client_dbaccess_path, *fsaccess)),
#else
                 NULL,
#endif
#ifdef GFX_CLASS
                 &gfx,
#else
                 NULL,
#endif
                 "N9tSBJDC",
                 USER_AGENT.c_str(),
                 THREADS_PER_MEGACLIENT)
        , clientname(name)
        , fsBasePath(basepath / fs::u8path(name))
        , resultproc(*this)
        , clientthread([this]() { threadloop(); })
    {
        client.clientname = clientname + " ";
#ifdef GFX_CLASS
        gfx.startProcessingThread();
#endif
    }

    ~StandardClient()
    {
        // shut down any syncs on the same thread, or they stall the client destruction (CancelIo instead of CancelIoEx on the WinDirNotify)
        auto result =
          thread_do<bool>([](MegaClient& mc, PromiseBoolSP result)
                          {
                              mc.logout(false);
                              result->set_value(true);
                          });

        // Make sure logout completes before we escape.
        result.get();

        clientthreadexit = true;
        waiter.notify();
        clientthread.join();
    }

    void localLogout()
    {
        auto result =
          thread_do<bool>([](MegaClient& mc, PromiseBoolSP result)
                          {
                              mc.locallogout(false, true);
                              result->set_value(true);
                          });

        // Make sure logout completes before we escape.
        result.get();
    }

    static mutex om;
    bool logcb = false;
    chrono::steady_clock::time_point lastcb = std::chrono::steady_clock::now();

    string lp(LocalNode* ln) { return ln->getLocalPath().toName(*client.fsaccess, FS_UNKNOWN); }

    void onCallback() { lastcb = chrono::steady_clock::now(); };

    void syncupdate_stateconfig(const SyncConfig& config) override { onCallback(); if (logcb) { lock_guard<mutex> g(om);  out() << clientname << " syncupdate_stateconfig() " << config.mBackupId; } }
    void syncupdate_scanning(bool b) override { if (logcb) { onCallback(); lock_guard<mutex> g(om); out() << clientname << " syncupdate_scanning()" << b; } }
    void syncupdate_local_lockretry(bool b) override { if (logcb) { onCallback(); lock_guard<mutex> g(om); out() << clientname << "syncupdate_local_lockretry() " << b; }}
    //void syncupdate_treestate(LocalNode* ln) override { onCallback(); if (logcb) { lock_guard<mutex> g(om);   out() << clientname << " syncupdate_treestate() " << ln->ts << " " << ln->dts << " " << lp(ln); }}

    bool sync_syncable(Sync* sync, const char* name, LocalPath& path, Node*) override
    {
        return sync_syncable(sync, name, path);
    }

    bool sync_syncable(Sync*, const char*, LocalPath&) override
    {
        onCallback();

        return true;
    }

    std::atomic<unsigned> transfersAdded{0}, transfersRemoved{0}, transfersPrepared{0}, transfersFailed{0}, transfersUpdated{0}, transfersComplete{0};

    void transfer_added(Transfer*) override { onCallback(); ++transfersAdded; }
    void transfer_removed(Transfer*) override { onCallback(); ++transfersRemoved; }
    void transfer_prepare(Transfer*) override { onCallback(); ++transfersPrepared; }
    void transfer_failed(Transfer*,  const Error&, dstime = 0) override { onCallback(); ++transfersFailed; }
    void transfer_update(Transfer*) override { onCallback(); ++transfersUpdated; }
    void transfer_complete(Transfer*) override { onCallback(); ++transfersComplete; }

    void notify_retry(dstime t, retryreason_t r) override
    {
        onCallback();

        if (!logcb) return;

        lock_guard<mutex> guard(om);

        out() << clientname << " notify_retry: " << t << " " << r;
    }

    void request_error(error e) override
    {
        onCallback();

        if (!logcb) return;

        lock_guard<mutex> guard(om);

        out() << clientname << " request_error: " << e;
    }

    void request_response_progress(m_off_t a, m_off_t b) override
    {
        onCallback();

        if (!logcb) return;

        lock_guard<mutex> guard(om);

        out() << clientname << " request_response_progress: " << a << " " << b;
    }

    void threadloop()
        try
    {
        while (!clientthreadexit)
        {
            int r;

            {
                std::lock_guard<std::recursive_mutex> lg(clientMutex);
                r = client.preparewait();
            }

            if (!r)
            {
                r |= client.dowait();
            }

            std::lock_guard<std::recursive_mutex> lg(clientMutex);
            r |= client.checkevents();

            {
                std::lock_guard<mutex> g(functionDoneMutex);
                if (nextfunctionMC)
                {
                    nextfunctionMC();
                    nextfunctionMC = nullptr;
                    functionDone.notify_all();
                    r |= Waiter::NEEDEXEC;
                }
                if (nextfunctionSC)
                {
                    nextfunctionSC();
                    nextfunctionSC = nullptr;
                    functionDone.notify_all();
                    r |= Waiter::NEEDEXEC;
                }
            }
            if ((r & Waiter::NEEDEXEC))
            {
                client.exec();
            }
        }
        out() << clientname << " thread exiting naturally";
    }
    catch (std::exception& e)
    {
        out() << clientname << " thread exception, StandardClient " << clientname << " terminated: " << e.what();
    }
    catch (...)
    {
        out() << clientname << " thread exception, StandardClient " << clientname << " terminated";
    }

    static bool debugging;  // turn this on to prevent the main thread timing out when stepping in the MegaClient

    template <class PROMISE_VALUE>
    future<PROMISE_VALUE> thread_do(std::function<void(MegaClient&, shared_promise<PROMISE_VALUE>)> f)
    {
        unique_lock<mutex> guard(functionDoneMutex);
        std::shared_ptr<promise<PROMISE_VALUE>> promiseSP(new promise<PROMISE_VALUE>());
        nextfunctionMC = [this, promiseSP, f](){ f(this->client, promiseSP); };
        waiter.notify();
        while (!functionDone.wait_until(guard, chrono::steady_clock::now() + chrono::seconds(600), [this]() { return !nextfunctionMC; }))
        {
            if (!debugging)
            {
                promiseSP->set_value(PROMISE_VALUE());
                break;
            }
        }
        return promiseSP->get_future();
    }

    template <class PROMISE_VALUE>
    future<PROMISE_VALUE> thread_do(std::function<void(StandardClient&, shared_promise<PROMISE_VALUE>)> f)
    {
        unique_lock<mutex> guard(functionDoneMutex);
        std::shared_ptr<promise<PROMISE_VALUE>> promiseSP(new promise<PROMISE_VALUE>());
        nextfunctionMC = [this, promiseSP, f]() { f(*this, promiseSP); };
        waiter.notify();
        while (!functionDone.wait_until(guard, chrono::steady_clock::now() + chrono::seconds(600), [this]() { return !nextfunctionSC; }))
        {
            if (!debugging)
            {
                promiseSP->set_value(PROMISE_VALUE());
                break;
            }
        }
        return promiseSP->get_future();
    }

    void preloginFromEnv(const string& userenv, PromiseBoolSP pb)
    {
        string user = getenv(userenv.c_str());

        ASSERT_FALSE(user.empty());

        resultproc.prepresult(PRELOGIN, ++next_request_tag,
            [&](){ client.prelogin(user.c_str()); },
            [pb](error e) { pb->set_value(!e); return true; });

    }

    void loginFromEnv(const string& userenv, const string& pwdenv, PromiseBoolSP pb)
    {
        string user = getenv(userenv.c_str());
        string pwd = getenv(pwdenv.c_str());

        ASSERT_FALSE(user.empty());
        ASSERT_FALSE(pwd.empty());

        byte pwkey[SymmCipher::KEYLENGTH];

        resultproc.prepresult(LOGIN, ++next_request_tag,
            [&](){
                if (client.accountversion == 1)
                {
                    if (error e = client.pw_key(pwd.c_str(), pwkey))
                    {
                        ASSERT_TRUE(false) << "login error: " << e;
                    }
                    else
                    {
                        client.login(user.c_str(), pwkey);
                    }
                }
                else if (client.accountversion == 2 && !salt.empty())
                {
                    client.login2(user.c_str(), pwd.c_str(), &salt);
                }
                else
                {
                    ASSERT_TRUE(false) << "Login unexpected error";
                }
            },
            [pb](error e) { pb->set_value(!e); return true; });

    }

    void loginFromSession(const string& session, PromiseBoolSP pb)
    {
        resultproc.prepresult(LOGIN, ++next_request_tag,
            [&](){ client.login(session); },
            [pb](error e) { pb->set_value(!e);  return true; });
    }

    bool cloudCopyTreeAs(Node* from, Node* to, string name)
    {
        auto promise = newPromiseBoolSP();
        auto future = promise->get_future();

        cloudCopyTreeAs(from, to, std::move(name), std::move(promise));

        return future.get();
    }

    class BasicPutNodesCompletion
    {
    public:
        BasicPutNodesCompletion(std::function<void(const Error&)>&& callable)
            : mCallable(std::move(callable))
        {
        }

        void operator()(const Error& e, targettype_t, vector<NewNode>&, bool)
        {
            mCallable(e);
        }

    private:
        std::function<void(const Error&)> mCallable;
    }; // BasicPutNodesCompletion

    void cloudCopyTreeAs(Node* n1, Node* n2, std::string newname, PromiseBoolSP pb)
    {
        auto completion = BasicPutNodesCompletion([pb](const Error& e) {
            pb->set_value(!e);
        });

        resultproc.prepresult(COMPLETION, ++next_request_tag,
            [&](){
                TreeProcCopy tc;
                client.proctree(n1, &tc, false, true);
                tc.allocnodes();
                client.proctree(n1, &tc, false, true);
                tc.nn[0].parenthandle = UNDEF;

                SymmCipher key;
                AttrMap attrs;
                string attrstring;
                key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
                attrs = n1->attrs;
                client.fsaccess->normalize(&newname);
                attrs.map['n'] = newname;
                attrs.getjson(&attrstring);
                client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
                client.putnodes(n2->nodeHandle(), move(tc.nn), nullptr, 0, std::move(completion));
            },
            nullptr);
    }

    void putnodes(NodeHandle parentHandle, std::vector<NewNode>&& nodes, PromiseBoolSP pb)
    {
        auto completion = BasicPutNodesCompletion([pb](const Error& e) {
            pb->set_value(!e);
        });

        resultproc.prepresult(COMPLETION,
                              ++next_request_tag,
                              [&]()
                              {
                                  client.putnodes(parentHandle, std::move(nodes), nullptr, 0, std::move(completion));
                              },
                              nullptr);
    }

    bool putnodes(NodeHandle parentHandle, std::vector<NewNode>&& nodes)
    {
        auto result =
          thread_do<bool>([&](StandardClient& client, PromiseBoolSP pb)
                    {
                        client.putnodes(parentHandle, std::move(nodes), pb);
                    });

        return result.get();
    }

    void uploadFolderTree_recurse(handle parent, handle& h, const fs::path& p, vector<NewNode>& newnodes)
    {
        NewNode n;
        client.putnodes_prepareOneFolder(&n, p.filename().u8string());
        handle thishandle = n.nodehandle = h++;
        n.parenthandle = parent;
        newnodes.emplace_back(std::move(n));

        for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
        {
            if (fs::is_directory(*i))
            {
                uploadFolderTree_recurse(thishandle, h, *i, newnodes);
            }
        }
    }

    void uploadFolderTree(fs::path p, Node* n2, PromiseBoolSP pb)
    {
        auto completion = BasicPutNodesCompletion([pb](const Error& e) {
            pb->set_value(!e);
        });

        resultproc.prepresult(COMPLETION, ++next_request_tag,
            [&](){
                vector<NewNode> newnodes;
                handle h = 1;
                uploadFolderTree_recurse(UNDEF, h, p, newnodes);
                client.putnodes(n2->nodeHandle(), move(newnodes), nullptr, 0, std::move(completion));
            },
            nullptr);
    }

    // Necessary to make sure we release the file once we're done with it.
    struct FileGet : public File {
        void completed(Transfer* t, LocalNode* n) override
        {
            File::completed(t, n);
            result->set_value(true);
            delete this;
        }

        void terminated() override
        {
            result->set_value(false);
            delete this;
        }

        PromiseBoolSP result;
    }; // FileGet

    void downloadFile(const Node& node, const fs::path& destination, PromiseBoolSP result)
    {
        unique_ptr<FileGet> file(new FileGet());

        file->h = node.nodeHandle();
        file->hprivate = true;
        file->localname = LocalPath::fromPath(destination.u8string(), *client.fsaccess);
        file->name = node.displayname();
        file->result = std::move(result);

        reinterpret_cast<FileFingerprint&>(*file) = node;

        DBTableTransactionCommitter committer(client.tctable);
        client.startxfer(GET, file.release(), committer);
    }

    bool downloadFile(const Node& node, const fs::path& destination)
    {
        auto result =
          thread_do<bool>([&](StandardClient& client, PromiseBoolSP result)
                          {
                              client.downloadFile(node, destination, result);
                          });

        return result.get();
    }

    struct FilePut : public File {
        void completed(Transfer* t, LocalNode* n) override
        {
            File::completed(t, n);
            delete this;
        }

        void terminated() override
        {
            delete this;
        }
    }; // FilePut

    bool uploadFolderTree(fs::path p, Node* n2)
    {
        auto promise = newPromiseBoolSP();
        auto future = promise->get_future();

        uploadFolderTree(p, n2, std::move(promise));

        return future.get();
    }

    void uploadFile(const fs::path& path, const string& name, Node* parent, DBTableTransactionCommitter& committer)
    {
        unique_ptr<File> file(new FilePut());

        file->h = parent->nodeHandle();
        file->localname = LocalPath::fromPath(path.u8string(), *client.fsaccess);
        file->name = name;

        client.startxfer(PUT, file.release(), committer);
    }

    void uploadFile(const fs::path& path, const string& name, Node* parent, PromiseBoolSP pb)
    {
        resultproc.prepresult(PUTNODES,
                              ++next_request_tag,
                              [&]()
                              {
                                  DBTableTransactionCommitter committer(client.tctable);
                                  uploadFile(path, name, parent, committer);
                              },
                              [pb](error e)
                              {
                                  pb->set_value(!e);
                                  return true;
                              });
    }

    bool uploadFile(const fs::path& path, const string& name, Node* parent)
    {
        auto result =
          thread_do<bool>([&](StandardClient& client, PromiseBoolSP pb)
                    {
                        client.uploadFile(path, name, parent, pb);
                    });

        return result.get();
    }

    bool uploadFile(const fs::path& path, Node* parent)
    {
        return uploadFile(path, path.filename().u8string(), parent);
    }

    void uploadFilesInTree_recurse(Node* target, const fs::path& p, std::atomic<int>& inprogress, DBTableTransactionCommitter& committer)
    {
        if (fs::is_regular_file(p))
        {
            ++inprogress;
            uploadFile(p, p.filename().u8string(), target, committer);
        }
        else if (fs::is_directory(p))
        {
            if (auto newtarget = client.childnodebyname(target, p.filename().u8string().c_str()))
            {
                for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
                {
                    uploadFilesInTree_recurse(newtarget, *i, inprogress, committer);
                }
            }
        }
    }

    bool uploadFilesInTree(fs::path p, Node* n2)
    {
        auto promise = newPromiseBoolSP();
        auto future = promise->get_future();

        std::atomic_int dummy(0);
        uploadFilesInTree(p, n2, dummy, std::move(promise));

        return future.get();
    }

    void uploadFilesInTree(fs::path p, Node* n2, std::atomic<int>& inprogress, PromiseBoolSP pb)
    {
        resultproc.prepresult(PUTNODES, ++next_request_tag,
            [&](){
                DBTableTransactionCommitter committer(client.tctable);
                uploadFilesInTree_recurse(n2, p, inprogress, committer);
            },
            [pb, &inprogress](error e)
            {
                if (!--inprogress)
                    pb->set_value(true);
                return !inprogress;
            });
    }



    class TreeProcPrintTree : public TreeProc
    {
    public:
        void proc(MegaClient* client, Node* n) override
        {
            //out() << "fetchnodes tree: " << n->displaypath();;
        }
    };

    // mark node as removed and notify

    std::function<void (StandardClient& mc, PromiseBoolSP pb)> onFetchNodes;

    void fetchnodes(bool noCache, PromiseBoolSP pb)
    {
        resultproc.prepresult(FETCHNODES, ++next_request_tag,
            [&](){ client.fetchnodes(noCache); },
            [this, pb](error e)
            {
                if (e)
                {
                    pb->set_value(false);
                }
                else
                {
                    TreeProcPrintTree tppt;
                    client.proctree(client.nodebyhandle(client.rootnodes[0]), &tppt);

                    if (onFetchNodes)
                    {
                        onFetchNodes(*this, pb);
                    }
                    else
                    {
                        pb->set_value(true);
                    }
                }
                onFetchNodes = nullptr;
                return true;
            });
    }

    bool fetchnodes(bool noCache = false)
    {
        auto result =
          thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                          {
                              client.fetchnodes(noCache, result);
                          });

        return result.get();
    }

    NewNode makeSubfolder(const string& utf8Name)
    {
        NewNode newnode;
        client.putnodes_prepareOneFolder(&newnode, utf8Name);
        return newnode;
    }

    void catchup(PromiseBoolSP pb)
    {
        resultproc.prepresult(CATCHUP, ++next_request_tag,
            [&](){
                client.catchup();
            },
            [pb](error e) {
                if (e)
                {
                    out() << "catchup reports: " << e;
                }
                pb->set_value(!e);
                return true;
            });
    }

    void deleteTestBaseFolder(bool mayneeddeleting, PromiseBoolSP pb)
    {
        if (Node* root = client.nodebyhandle(client.rootnodes[0]))
        {
            if (Node* basenode = client.childnodebyname(root, "mega_test_sync", false))
            {
                if (mayneeddeleting)
                {
                    auto completion = [this, pb](NodeHandle, Error e) {
                        if (e) out() << "delete of test base folder reply reports: " << e;
                        deleteTestBaseFolder(false, pb);
                    };

                    resultproc.prepresult(COMPLETION, ++next_request_tag,
                        [&](){ client.unlink(basenode, false, 0, std::move(completion)); },
                        nullptr);
                    return;
                }
                out() << "base folder found, but not expected, failing";
                pb->set_value(false);
                return;
            }
            else
            {
                //out() << "base folder not found, wasn't present or delete successful";
                pb->set_value(true);
                return;
            }
        }
        out() << "base folder not found, as root was not found!";
        pb->set_value(false);
    }

    void ensureTestBaseFolder(bool mayneedmaking, PromiseBoolSP pb)
    {
        if (Node* root = client.nodebyhandle(client.rootnodes[0]))
        {
            if (Node* basenode = client.childnodebyname(root, "mega_test_sync", false))
            {
                if (basenode->type == FOLDERNODE)
                {
                    basefolderhandle = basenode->nodehandle;
                    //out() << clientname << " Base folder: " << Base64Str<MegaClient::NODEHANDLE>(basefolderhandle);
                    //parentofinterest = Base64Str<MegaClient::NODEHANDLE>(basefolderhandle);
                    pb->set_value(true);
                    return;
                }
            }
            else if (mayneedmaking)
            {
                vector<NewNode> nn(1);
                nn[0] = makeSubfolder("mega_test_sync");

                auto completion = BasicPutNodesCompletion([this, pb](const Error&) {
                    ensureTestBaseFolder(false, pb);
                });

                resultproc.prepresult(COMPLETION, ++next_request_tag,
                    [&](){ client.putnodes(root->nodeHandle(), move(nn), nullptr, 0, std::move(completion)); },
                    nullptr);

                return;
            }
        }
        pb->set_value(false);
    }

    NewNode* buildSubdirs(list<NewNode>& nodes, const string& prefix, int n, int recurselevel)
    {
        nodes.emplace_back(makeSubfolder(prefix));
        auto& nn = nodes.back();
        nn.nodehandle = nodes.size();

        if (recurselevel > 0)
        {
            for (int i = 0; i < n; ++i)
            {
                buildSubdirs(nodes, prefix + "_" + to_string(i), n, recurselevel - 1)->parenthandle = nn.nodehandle;
            }
        }

        return &nn;
    }

    bool makeCloudSubdirs(const string& prefix, int depth, int fanout)
    {
        auto result =
          thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                          {
                              client.makeCloudSubdirs(prefix, depth, fanout, result);
                          });

        return result.get();
    }

    void makeCloudSubdirs(const string& prefix, int depth, int fanout, PromiseBoolSP pb, const string& atpath = "")
    {
        assert(basefolderhandle != UNDEF);

        std::list<NewNode> nodes;
        NewNode* nn = buildSubdirs(nodes, prefix, fanout, depth);
        nn->parenthandle = UNDEF;
        nn->ovhandle = UNDEF;

        Node* atnode = client.nodebyhandle(basefolderhandle);
        if (atnode && !atpath.empty())
        {
            atnode = drillchildnodebyname(atnode, atpath);
        }
        if (!atnode)
        {
            out() << "path not found: " << atpath;
            pb->set_value(false);
        }
        else
        {
            auto nodearray = vector<NewNode>(nodes.size());
            size_t i = 0;
            for (auto n = nodes.begin(); n != nodes.end(); ++n, ++i)
            {
                nodearray[i] = std::move(*n);
            }

            auto completion = [pb, this](const Error& e, targettype_t, vector<NewNode>& nodes, bool) {
                lastPutnodesResultFirstHandle = nodes.empty() ? UNDEF : nodes[0].mAddedHandle;
                pb->set_value(!e);
            };

            resultproc.prepresult(COMPLETION, ++next_request_tag,
                [&]() {
                    client.putnodes(atnode->nodeHandle(), move(nodearray), nullptr, 0, std::move(completion));
                },
                nullptr);
        }
    }

    struct SyncInfo
    {
        NodeHandle h;
        fs::path localpath;
    };

    SyncConfig syncConfigByBackupID(handle backupID) const
    {
        auto* config = client.syncs.syncConfigByBackupId(backupID);

        assert(config);

        return *config;
    }

    bool syncSet(handle backupId, SyncInfo& info) const
    {
        if (auto* config = client.syncs.syncConfigByBackupId(backupId))
        {
            info.h = config->getRemoteNode();
            info.localpath = config->getLocalPath().toPath(*client.fsaccess);

            return true;
        }

        return false;
    }

    SyncInfo syncSet(handle backupId)
    {
        SyncInfo result;

        out() << "looking up id " << backupId;

        client.syncs.forEachUnifiedSync([](UnifiedSync& us){
            out() << " ids are: " << us.mConfig.mBackupId << " with local path '" << us.mConfig.getLocalPath().toPath(*us.mClient.fsaccess);
        });

        bool found = syncSet(backupId, result);
        assert(found);

        return result;
    }

    SyncInfo syncSet(handle backupId) const
    {
        return const_cast<StandardClient&>(*this).syncSet(backupId);
    }

    Node* getcloudrootnode()
    {
        return client.nodebyhandle(client.rootnodes[0]);
    }

    Node* gettestbasenode()
    {
        return client.childnodebyname(getcloudrootnode(), "mega_test_sync", false);
    }

    Node* getcloudrubbishnode()
    {
        return client.nodebyhandle(client.rootnodes[RUBBISHNODE - ROOTNODE]);
    }

    Node* drillchildnodebyname(Node* n, const string& path)
    {
        for (size_t p = 0; n && p < path.size(); )
        {
            auto pos = path.find("/", p);
            if (pos == string::npos) pos = path.size();
            n = client.childnodebyname(n, path.substr(p, pos - p).c_str(), false);
            p = pos == string::npos ? path.size() : pos + 1;
        }
        return n;
    }

    vector<Node*> drillchildnodesbyname(Node* n, const string& path)
    {
        auto pos = path.find("/");
        if (pos == string::npos)
        {
            return client.childnodesbyname(n, path.c_str(), false);
        }
        else
        {
            vector<Node*> results, subnodes = client.childnodesbyname(n, path.c_str(), false);
            for (size_t i = subnodes.size(); i--; )
            {
                if (subnodes[i]->type != FILENODE)
                {
                    vector<Node*> v = drillchildnodesbyname(subnodes[i], path.substr(pos + 1));
                    results.insert(results.end(), v.begin(), v.end());
                }
            }
            return results;
        }
    }

    bool backupAdd_inthread(const string& drivePath,
                            string sourcePath,
                            const string& targetPath,
                            SyncCompletionFunction completion)
    {
        auto* rootNode = client.nodebyhandle(basefolderhandle);

        // Root isn't in the cloud.
        if (!rootNode)
        {
            return false;
        }

        auto* targetNode = drillchildnodebyname(rootNode, targetPath);

        // Target path doesn't exist.
        if (!targetNode)
        {
            return false;
        }

        // Generate drive ID if necessary.
        auto id = UNDEF;
        auto result = client.readDriveId(drivePath.c_str(), id);

        if (result == API_ENOENT)
        {
            id = client.generateDriveId();
            result = client.writeDriveId(drivePath.c_str(), id);
        }

        if (result != API_OK)
        {
            completion(nullptr, NO_SYNC_ERROR, result);
            return false;
        }

        auto config =
          SyncConfig(LocalPath::fromPath(sourcePath, *client.fsaccess),
                     sourcePath,
                     targetNode->nodeHandle(),
                     targetPath,
                     0,
                     LocalPath::fromPath(drivePath, *client.fsaccess),
                     //string_vector(),
                     true,
                     SyncConfig::TYPE_BACKUP);

        // Try and add the backup.
        return client.addsync(config, true, completion) == API_OK;
    }

    handle backupAdd_mainthread(const string& drivePath,
                                const string& sourcePath,
                                const string& targetPath)
    {
        const fs::path dp = fsBasePath / fs::u8path(drivePath);
        const fs::path sp = fsBasePath / fs::u8path(sourcePath);

        fs::create_directories(dp);
        fs::create_directories(sp);

        auto result =
          thread_do<handle>(
            [&](StandardClient& client, PromiseHandleSP result)
            {
                auto completion =
                  [=](UnifiedSync* us, const SyncError& se, error e)
                  {
                    auto success = !!us && !se && !e;
                    result->set_value(success ? us->mConfig.mBackupId : UNDEF);
                  };

                  client.backupAdd_inthread(dp.u8string(),
                                            sp.u8string(),
                                            targetPath,
                                            std::move(completion));
            });

        return result.get();
    }

    bool setupSync_inthread(const string& subfoldername, const fs::path& localpath, const bool isBackup,
                            SyncCompletionFunction addSyncCompletion)
    {
        if (Node* n = client.nodebyhandle(basefolderhandle))
        {
            if (Node* m = drillchildnodebyname(n, subfoldername))
            {
                out() << clientname << "Setting up sync from " << m->displaypath() << " to " << localpath;
                auto syncConfig =
                    SyncConfig(LocalPath::fromPath(localpath.u8string(), *client.fsaccess),
                               localpath.u8string(),
                               NodeHandle().set6byte(m->nodehandle),
                               subfoldername,
                               0,
                               LocalPath(),
                               //string_vector(),
                               true,
                               isBackup ? SyncConfig::TYPE_BACKUP : SyncConfig::TYPE_TWOWAY);

                error e = client.addsync(syncConfig, true, addSyncCompletion);
                return !e;
            }
        }
        assert(false);
        return false;
    }

    void importSyncConfigs(string configs, PromiseBoolSP result)
    {
        auto completion = [result](error e) { result->set_value(!e); };
        client.importSyncConfigs(configs.c_str(), std::move(completion));
    }

    bool importSyncConfigs(string configs)
    {
        auto result =
          thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                          {
                              client.importSyncConfigs(configs, result);
                          });

        return result.get();
    }

    string exportSyncConfigs()
    {
        auto result =
          thread_do<string>([](MegaClient& client, PromiseStringSP result)
                            {
                                auto configs = client.syncs.exportSyncConfigs();
                                result->set_value(configs);
                            });

        return result.get();
    }

    bool delSync_inthread(handle backupId, const bool keepCache)
    {
        const auto handle = syncSet(backupId).h;
        bool removed = false;

        client.syncs.removeSelectedSyncs(
          [&](SyncConfig& c, Sync*)
          {
              const bool matched = c.getRemoteNode() == handle;

              removed |= matched;

              return matched;
          });

        return removed;
    }

    struct CloudNameLess
    {
        bool operator()(const string& lhs, const string& rhs) const
        {
            return compare(lhs, rhs) < 0;
        }

        static int compare(const string& lhs, const string& rhs)
        {
            return compareUtf(lhs, false, rhs, false, false);
        }

        static bool equal(const string& lhs, const string& rhs)
        {
            return compare(lhs, rhs) == 0;
        }
    }; // CloudNameLess

    bool recursiveConfirm(Model::ModelNode* mn, Node* n, int& descendants, const string& identifier, int depth, bool& firstreported)
    {
        // top level names can differ so we don't check those
        if (!mn || !n) return false;

        if (depth)
        {
            if (!CloudNameLess().equal(mn->cloudName(), n->displayname()))
            {
                out() << "Node name mismatch: " << mn->path() << " " << n->displaypath();
                return false;
            }
        }

        if (!mn->typematchesnodetype(n->type))
        {
            out() << "Node type mismatch: " << mn->path() << ":" << mn->type << " " << n->displaypath() << ":" << n->type;
            return false;
        }

        if (n->type == FILENODE)
        {
            // not comparing any file versioning (for now)
            return true;
        }

        multimap<string, Model::ModelNode*, CloudNameLess> ms;
        multimap<string, Node*, CloudNameLess> ns;
        for (auto& m : mn->kids)
        {
            ms.emplace(m->cloudName(), m.get());
        }
        for (auto& n2 : n->children)
        {
            ns.emplace(n2->displayname(), n2);
        }

        int matched = 0;
        vector<string> matchedlist;
        for (auto m_iter = ms.begin(); m_iter != ms.end(); )
        {
            if (!depth && m_iter->first == DEBRISFOLDER)
            {
                m_iter = ms.erase(m_iter); // todo: add checks of the remote debris folder later
                continue;
            }

            auto er = ns.equal_range(m_iter->first);
            auto next_m = m_iter;
            ++next_m;
            bool any_equal_matched = false;
            for (auto i = er.first; i != er.second; ++i)
            {
                int rdescendants = 0;
                if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, firstreported))
                {
                    ++matched;
                    matchedlist.push_back(m_iter->first);
                    ns.erase(i);
                    ms.erase(m_iter);
                    descendants += rdescendants;
                    any_equal_matched = true;
                    break;
                }
            }
            if (!any_equal_matched)
            {
                break;
            }
            m_iter = next_m;
        }
        if (ns.empty() && ms.empty())
        {
            descendants += matched;
            return true;
        }
        else if (!firstreported)
        {
            ostringstream ostream;
            firstreported = true;
            ostream << clientname << " " << identifier << " after matching " << matched << " child nodes [";
            for (auto& ml : matchedlist) ostream << ml << " ";
            ostream << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
            for (auto& m : ms) ostream << " " << m.first;
            ostream << " and unmatched remote nodes:";
            for (auto& i : ns) ostream << " " << i.first;
            out() << ostream.str();
        };
        return false;
    }

    bool localNodesMustHaveNodes = true;

    bool recursiveConfirm(Model::ModelNode* mn, LocalNode* n, int& descendants, const string& identifier, int depth, bool& firstreported)
    {
        // top level names can differ so we don't check those
        if (!mn || !n) return false;

        if (depth)
        {
            if (!CloudNameLess().equal(mn->cloudName(), n->name))
            {
                out() << "LocalNode name mismatch: " << mn->path() << " " << n->name;
                return false;
            }
        }

        if (!mn->typematchesnodetype(n->type))
        {
            out() << "LocalNode type mismatch: " << mn->path() << ":" << mn->type << " " << n->name << ":" << n->type;
            return false;
        }

        auto localpath = n->getLocalPath().toName(*client.fsaccess, FS_UNKNOWN);
        string n_localname = n->localname.toName(*client.fsaccess, FS_UNKNOWN);
        if (n_localname.size())
        {
            EXPECT_EQ(n->name, n_localname);
        }
        if (localNodesMustHaveNodes)
        {
            EXPECT_TRUE(n->node != nullptr);
        }
        if (depth && n->node)
        {
            EXPECT_EQ(n->node->displayname(), n->name);
        }
        if (depth && mn->parent)
        {
            EXPECT_EQ(mn->parent->type, Model::ModelNode::folder);
            EXPECT_EQ(n->parent->type, FOLDERNODE);

            string parentpath = n->parent->getLocalPath().toName(*client.fsaccess, FS_UNKNOWN);
            EXPECT_EQ(localpath.substr(0, parentpath.size()), parentpath);
        }
        if (n->node && n->parent && n->parent->node)
        {
            string p = n->node->displaypath();
            string pp = n->parent->node->displaypath();
            EXPECT_EQ(p.substr(0, pp.size()), pp);
            EXPECT_EQ(n->parent->node, n->node->parent);
        }

        multimap<string, Model::ModelNode*, CloudNameLess> ms;
        multimap<string, LocalNode*, CloudNameLess> ns;
        for (auto& m : mn->kids)
        {
            ms.emplace(m->cloudName(), m.get());
        }
        for (auto& n2 : n->children)
        {
            if (!n2.second->deleted) ns.emplace(n2.second->name, n2.second); // todo: should LocalNodes marked as deleted actually have been removed by now?
        }

        int matched = 0;
        vector<string> matchedlist;
        for (auto m_iter = ms.begin(); m_iter != ms.end(); )
        {
            if (!depth && m_iter->first == DEBRISFOLDER)
            {
                m_iter = ms.erase(m_iter); // todo: are there LocalNodes representing the trash?
                continue;
            }

            auto er = ns.equal_range(m_iter->first);
            auto next_m = m_iter;
            ++next_m;
            bool any_equal_matched = false;
            for (auto i = er.first; i != er.second; ++i)
            {
                int rdescendants = 0;
                if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, firstreported))
                {
                    ++matched;
                    matchedlist.push_back(m_iter->first);
                    ns.erase(i);
                    ms.erase(m_iter);
                    descendants += rdescendants;
                    any_equal_matched = true;
                    break;
                }
            }
            if (!any_equal_matched)
            {
                break;
            }
            m_iter = next_m;
        }
        if (ns.empty() && ms.empty())
        {
            return true;
        }
        else if (!firstreported)
        {
            ostringstream ostream;
            firstreported = true;
            ostream << clientname << " " << identifier << " after matching " << matched << " child nodes [";
            for (auto& ml : matchedlist) ostream << ml << " ";
            ostream << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
            for (auto& m : ms) ostream << " " << m.first;
            ostream << " and unmatched LocalNodes:";
            for (auto& i : ns) ostream << " " << i.first;
            out() << ostream.str();
        };
        return false;
    }


    bool recursiveConfirm(Model::ModelNode* mn, fs::path p, int& descendants, const string& identifier, int depth, bool ignoreDebris, bool& firstreported)
    {
        struct Comparator
        {
            bool operator()(const string& lhs, const string& rhs) const
            {
                return compare(lhs, rhs) < 0;
            }

            int compare(const string& lhs, const string& rhs) const
            {
                return compareUtf(lhs, true, rhs, true, false);
            }
        }; // Comparator

        static Comparator comparator;

        if (!mn) return false;

        if (depth)
        {
            if (comparator.compare(p.filename().u8string(), mn->fsName()))
            {
                out() << "filesystem name mismatch: " << mn->path() << " " << p;
                return false;
            }
        }
        nodetype_t pathtype = fs::is_directory(p) ? FOLDERNODE : fs::is_regular_file(p) ? FILENODE : TYPE_UNKNOWN;
        if (!mn->typematchesnodetype(pathtype))
        {
            out() << "Path type mismatch: " << mn->path() << ":" << mn->type << " " << p.u8string() << ":" << pathtype;
            return false;
        }

        if (pathtype == FILENODE && p.filename().u8string() != "lock")
        {
            if (localFSFilesThatMayDiffer.find(p) == localFSFilesThatMayDiffer.end())
            {
                ifstream fs(p, ios::binary);
                std::vector<char> buffer;
                buffer.resize(mn->content.size() + 1024);
                fs.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
                EXPECT_EQ(size_t(fs.gcount()), mn->content.size()) << " file is not expected size " << p;
                EXPECT_TRUE(!memcmp(buffer.data(), mn->content.data(), mn->content.size())) << " file data mismatch " << p;
            }
        }

        if (pathtype != FOLDERNODE)
        {
            return true;
        }

        multimap<string, Model::ModelNode*, Comparator> ms;
        multimap<string, fs::path, Comparator> ps;

        for (auto& m : mn->kids)
        {
            ms.emplace(m->fsName(), m.get());
        }

        for (fs::directory_iterator pi(p); pi != fs::directory_iterator(); ++pi)
        {
            ps.emplace(pi->path().filename().u8string(), pi->path());
        }

        if (ignoreDebris)
        {
            ms.erase(DEBRISFOLDER);
            ps.erase(DEBRISFOLDER);
        }

        int matched = 0;
        vector<string> matchedlist;
        for (auto m_iter = ms.begin(); m_iter != ms.end(); )
        {
            auto er = ps.equal_range(m_iter->first);
            auto next_m = m_iter;
            ++next_m;
            bool any_equal_matched = false;
            for (auto i = er.first; i != er.second; ++i)
            {
                int rdescendants = 0;
                if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, ignoreDebris, firstreported))
                {
                    ++matched;
                    matchedlist.push_back(m_iter->first);
                    ps.erase(i);
                    ms.erase(m_iter);
                    descendants += rdescendants;
                    any_equal_matched = true;
                    break;
                }
            }
            if (!any_equal_matched)
            {
                break;
            }
            m_iter = next_m;
        }
        //if (ps.size() == 1 && !mn->parent && ps.begin()->first == DEBRISFOLDER)
        //{
        //    ps.clear();
        //}
        if (ps.empty() && ms.empty())
        {
            return true;
        }
        else if (!firstreported)
        {
            ostringstream ostream;
            firstreported = true;
            ostream << clientname << " " << identifier << " after matching " << matched << " child nodes [";
            for (auto& ml : matchedlist) ostream << ml << " ";
            ostream << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
            for (auto& m : ms) ostream << " " << m.first;
            ostream << " and unmatched filesystem paths:";
            for (auto& i : ps) ostream << " " << i.second.filename();
            ostream << " in " << p;
            out() << ostream.str();
        };
        return false;
    }

    Sync* syncByBackupId(handle backupId)
    {
        return client.syncs.runningSyncByBackupId(backupId);
    }

    void enableSyncByBackupId(handle id, PromiseBoolSP result)
    {
        UnifiedSync* sync;
        result->set_value(!client.syncs.enableSyncByBackupId(id, false, sync));
    }

    bool enableSyncByBackupId(handle id)
    {
        auto result =
          thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                          {
                              client.enableSyncByBackupId(id, result);
                          });

        return result.get();
    }

    void backupIdForSyncPath(const fs::path& path, PromiseHandleSP result)
    {
        auto localPath = LocalPath::fromPath(path.u8string(), *client.fsaccess);
        auto id = UNDEF;

        client.syncs.forEachSyncConfig(
          [&](const SyncConfig& config)
          {
              if (config.mLocalPath != localPath) return;
              if (id != UNDEF) return;

              id = config.mBackupId;
          });

        result->set_value(id);
    }

    handle backupIdForSyncPath(fs::path path)
    {
        auto result =
          thread_do<handle>([=](StandardClient& client, PromiseHandleSP result)
                            {
                                client.backupIdForSyncPath(path, result);
                            });

        return result.get();
    }

    enum Confirm
    {
        CONFIRM_LOCALFS = 0x01,
        CONFIRM_LOCALNODE = 0x02,
        CONFIRM_LOCAL = CONFIRM_LOCALFS | CONFIRM_LOCALNODE,
        CONFIRM_REMOTE = 0x04,
        CONFIRM_ALL = CONFIRM_LOCAL | CONFIRM_REMOTE,
    };

    bool confirmModel_mainthread(handle id, Model::ModelNode* mRoot, Node* rRoot)
    {
        auto result =
          thread_do<bool>(
            [=](StandardClient& client, PromiseBoolSP result)
            {
                result->set_value(client.confirmModel(id, mRoot, rRoot));
            });

        return result.get();
    }

    bool confirmModel_mainthread(handle id, Model::ModelNode* mRoot, LocalNode* lRoot)
    {
        auto result =
          thread_do<bool>(
            [=](StandardClient& client, PromiseBoolSP result)
            {
                result->set_value(client.confirmModel(id, mRoot, lRoot));
            });

        return result.get();
    }

    bool confirmModel_mainthread(handle id, Model::ModelNode* mRoot, fs::path lRoot, const bool ignoreDebris = false)
    {
        auto result =
          thread_do<bool>(
            [=](StandardClient& client, PromiseBoolSP result)
            {
                result->set_value(client.confirmModel(id, mRoot, lRoot, ignoreDebris));
            });

        return result.get();
    }

    bool confirmModel(handle id, Model::ModelNode* mRoot, Node* rRoot)
    {
        string name = "Sync " + toHandle(id);
        int descendents = 0;
        bool reported = false;

        if (!recursiveConfirm(mRoot, rRoot, descendents, name, 0, reported))
        {
            out() << clientname << " syncid " << toHandle(id) << " comparison against remote nodes failed";
            return false;
        }

        return true;
    }

    bool confirmModel(handle id, Model::ModelNode* mRoot, LocalNode* lRoot)
    {
        string name = "Sync " + toHandle(id);
        int descendents = 0;
        bool reported = false;

        if (!recursiveConfirm(mRoot, lRoot, descendents, name, 0, reported))
        {
            out() << clientname << " syncid " << toHandle(id) << " comparison against LocalNodes failed";
            return false;
        }

        return true;
    }

    bool confirmModel(handle id, Model::ModelNode* mRoot, fs::path lRoot, const bool ignoreDebris = false)
    {
        string name = "Sync " + toHandle(id);
        int descendents = 0;
        bool reported = false;

        if (!recursiveConfirm(mRoot, lRoot, descendents, name, 0, ignoreDebris, reported))
        {
            out() << clientname << " syncid " << toHandle(id) << " comparison against local filesystem failed";
            return false;
        }

        return true;
    }

    bool confirmModel(handle backupId, Model::ModelNode* mnode, const int confirm, const bool ignoreDebris)
    {
        SyncInfo si;

        if (!syncSet(backupId, si))
        {
            out() << clientname << " backupId " << toHandle(backupId) << " not found ";
            return false;
        }

        // compare model against nodes representing remote state
        if ((confirm & CONFIRM_REMOTE) && !confirmModel(backupId, mnode, client.nodeByHandle(si.h)))
        {
            return false;
        }

        // compare model against LocalNodes
        if (Sync* sync = syncByBackupId(backupId))
        {
            if ((confirm & CONFIRM_LOCALNODE) && !confirmModel(backupId, mnode, sync->localroot.get()))
            {
                return false;
            }
        }

        // compare model against local filesystem
        if ((confirm & CONFIRM_LOCALFS) && !confirmModel(backupId, mnode, si.localpath, ignoreDebris))
        {
            return false;
        }

        return true;
    }

    void prelogin_result(int, string*, string* salt, error e) override
    {
        out() << clientname << " Prelogin: " << e;
        if (!e)
        {
            this->salt = *salt;
        }
        resultproc.processresult(PRELOGIN, e, UNDEF);
    }

    void login_result(error e) override
    {
        out() << clientname << " Login: " << e;
        resultproc.processresult(LOGIN, e, UNDEF);
    }

    void fetchnodes_result(const Error& e) override
    {
        out() << clientname << " Fetchnodes: " << e;
        resultproc.processresult(FETCHNODES, e, UNDEF);
    }

    bool setattr(Node* node, attr_map&& updates)
    {
        auto result =
          thread_do<bool>(
            [=](StandardClient& client, PromiseBoolSP result) mutable
            {
                client.setattr(node, std::move(updates), result);
            });

        return result.get();
    }

    void setattr(Node* node, attr_map&& updates, PromiseBoolSP result)
    {
        resultproc.prepresult(COMPLETION,
                              ++next_request_tag,
                              [=]()
                              {
                                  client.setattr(node, attr_map(updates), client.reqtag, nullptr,
                                      [result](NodeHandle, error e) { result->set_value(!e); });
                              }, nullptr);
    }

    void unlink_result(handle h, error e) override
    {
        resultproc.processresult(UNLINK, e, h);
    }

    handle lastPutnodesResultFirstHandle = UNDEF;

    void putnodes_result(const Error& e, targettype_t tt, vector<NewNode>& nn, bool targetOverride) override
    {
        resultproc.processresult(PUTNODES, e, client.restag);
    }

    void catchup_result() override
    {
        resultproc.processresult(CATCHUP, error(API_OK));
    }

    void disableSync(handle id, SyncError error, bool enabled, PromiseBoolSP result)
    {
        client.syncs.disableSelectedSyncs(
            [id](SyncConfig& config, Sync*)
            {
                return config.mBackupId == id;
            },
            false,
            error,
            enabled,
            [result](size_t nDisabled){
                result->set_value(!!nDisabled);
            });
    }

    bool disableSync(handle id, SyncError error, bool enabled)
    {
        auto result =
            thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                            {
                                client.disableSync(id, error, enabled, result);
                            });

        return result.get();
    }


    void deleteremote(string path, PromiseBoolSP pb)
    {
        if (Node* n = drillchildnodebyname(gettestbasenode(), path))
        {
            auto completion = [pb](NodeHandle, Error e) {
                pb->set_value(!e);
            };

            resultproc.prepresult(COMPLETION, ++next_request_tag,
                [&](){ client.unlink(n, false, 0, std::move(completion)); },
                nullptr);
        }
        else
        {
            pb->set_value(false);
        }
    }

    bool deleteremote(string path)
    {
        auto result =
          thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb)
                    {
                        sc.deleteremote(path, pb);
                    });

        return result.get();
    }

    void deleteremotenodes(vector<Node*> ns, PromiseBoolSP pb)
    {
        if (ns.empty())
        {
            pb->set_value(true);
        }
        else
        {
            for (size_t i = ns.size(); i--; )
            {
                auto completion = [i, pb](NodeHandle, Error e) {
                    if (!i) pb->set_value(!e);
                };

                resultproc.prepresult(COMPLETION, ++next_request_tag,
                    [&](){ client.unlink(ns[i], false, 0, std::move(completion)); },
                    nullptr);
            }
        }
    }

    bool movenode(string path, string newParentPath)
    {
        using std::future_status;

        auto promise = newPromiseBoolSP();
        auto future = promise->get_future();

        movenode(std::move(path),
                 std::move(newParentPath),
                 std::move(promise));

        auto status = future.wait_for(DEFAULTWAIT);

        return status == future_status::ready && future.get();
    }

    void movenode(string path, string newparentpath, PromiseBoolSP pb)
    {
        Node* n = drillchildnodebyname(gettestbasenode(), path);
        Node* p = drillchildnodebyname(gettestbasenode(), newparentpath);
        if (n && p)
        {
            resultproc.prepresult(COMPLETION, ++next_request_tag,
                [pb, n, p, this]()
                {
                    client.rename(n, p, SYNCDEL_NONE, NodeHandle(), nullptr,
                        [pb](NodeHandle h, Error e) { pb->set_value(!e); });
                },
                nullptr);
            return;
        }
        out() << "node or new parent not found";
        pb->set_value(false);
    }

    void movenode(handle h1, handle h2, PromiseBoolSP pb)
    {
        Node* n = client.nodebyhandle(h1);
        Node* p = client.nodebyhandle(h2);
        if (n && p)
        {
            resultproc.prepresult(COMPLETION, ++next_request_tag,
                [pb, n, p, this]()
                {
                    client.rename(n, p, SYNCDEL_NONE, NodeHandle(), nullptr,
                        [pb](NodeHandle h, Error e) { pb->set_value(!e); });
                },
                nullptr);
            return;
        }
        out() << "node or new parent not found by handle";
        pb->set_value(false);
    }

    void movenodetotrash(string path, PromiseBoolSP pb)
    {
        Node* n = drillchildnodebyname(gettestbasenode(), path);
        Node* p = getcloudrubbishnode();
        if (n && p && n->parent)
        {
            resultproc.prepresult(COMPLETION, ++next_request_tag,
                [pb, n, p, this]()
                {
                    client.rename(n, p, SYNCDEL_NONE, NodeHandle(), nullptr,
                        [pb](NodeHandle h, Error e) { pb->set_value(!e); });
                },
                nullptr);
            return;
        }
        out() << "node or rubbish or node parent not found";
        pb->set_value(false);
    }

    void exportnode(Node* n, int del, m_time_t expiry, bool writable, promise<Error>& pb)
    {
        resultproc.prepresult(COMPLETION, ++next_request_tag,
            [&](){
                error e = client.exportnode(n, del, expiry, writable, client.reqtag, [&](Error e, handle, handle){ pb.set_value(e); });
                if (e)
                {
                    pb.set_value(e);
                }
            }, nullptr);  // no need to match callbacks with requests when we use completion functions
    }

    void getpubliclink(Node* n, int del, m_time_t expiry, bool writable, promise<Error>& pb)
    {
        resultproc.prepresult(COMPLETION, ++next_request_tag,
            [&](){ client.requestPublicLink(n, del, expiry, writable, client.reqtag, [&](Error e, handle, handle){ pb.set_value(e); }); },
            nullptr);
    }


    void waitonsyncs(chrono::seconds d = chrono::seconds(2))
    {
        auto start = chrono::steady_clock::now();
        for (;;)
        {
            bool any_add_del = false;;
            vector<int> syncstates;

            thread_do<bool>([&syncstates, &any_add_del, this](StandardClient& mc, PromiseBoolSP pb)
            {
                mc.client.syncs.forEachRunningSync(
                  [&](Sync* s)
                  {
                      syncstates.push_back(s->state());
                      any_add_del |= !s->deleteq.empty();
                      any_add_del |= !s->insertq.empty();
                  });

                if (!(client.todebris.empty() && client.tounlink.empty() /*&& client.synccreate.empty()*/))
                {
                    any_add_del = true;
                }
                if (!client.transfers[GET].empty() || !client.transfers[PUT].empty())
                {
                    any_add_del = true;
                }
                pb->set_value(true);
            }).get();
            bool allactive = true;
            {
                lock_guard<mutex> g(StandardClient::om);
                //std::out() << "sync state: ";
                //for (auto n : syncstates)
                //{
                //    out() << n;
                //    if (n != SYNC_ACTIVE) allactive = false;
                //}
                //out();
            }

            if (any_add_del || debugging)
            {
                start = chrono::steady_clock::now();
            }

            if (allactive && ((chrono::steady_clock::now() - start) > d) && ((chrono::steady_clock::now() - lastcb) > d))
            {
               break;
            }
//out() << "waiting 500";
            WaitMillisec(500);
        }

    }

    bool login_reset(const string& user, const string& pw, bool noCache = false)
    {
        future<bool> p1;
        p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); });
        if (!waitonresults(&p1))
        {
            out() << "preloginFromEnv failed";
            return false;
        }
        p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); });
        if (!waitonresults(&p1))
        {
            out() << "loginFromEnv failed";
            return false;
        }
        p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(noCache, pb); });
        if (!waitonresults(&p1)) {
            out() << "fetchnodes failed";
            return false;
        }
        p1 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteTestBaseFolder(true, pb); });  // todo: do we need to wait for server response now
        if (!waitonresults(&p1)) {
            out() << "deleteTestBaseFolder failed";
            return false;
        }
        p1 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(true, pb); });
        if (!waitonresults(&p1)) {
            out() << "ensureTestBaseFolder failed";
            return false;
        }
        return true;
    }

    bool login_reset_makeremotenodes(const string& user, const string& pw, const string& prefix, int depth, int fanout, bool noCache = false)
    {
        if (!login_reset(user, pw, noCache))
        {
            out() << "login_reset failed";
            return false;
        }
        future<bool> p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs(prefix, depth, fanout, pb); });
        if (!waitonresults(&p1))
        {
            out() << "makeCloudSubdirs failed";
            return false;
        }
        return true;
    }

    void ensureSyncUserAttributes(PromiseBoolSP result)
    {
        auto completion = [result](Error e) { result->set_value(!e); };
        client.ensureSyncUserAttributes(std::move(completion));
    }

    bool ensureSyncUserAttributes()
    {
        auto result =
          thread_do<bool>([](StandardClient& client, PromiseBoolSP result)
                          {
                              client.ensureSyncUserAttributes(result);
                          });

        return result.get();
    }

    void copySyncConfig(SyncConfig config, PromiseHandleSP result)
    {
        auto completion =
          [result](handle id, error e)
          {
              result->set_value(e ? UNDEF : id);
          };

        client.copySyncConfig(config, std::move(completion));
    }

    handle copySyncConfig(const SyncConfig& config)
    {
        auto result =
          thread_do<handle>([=](StandardClient& client, PromiseHandleSP result)
                          {
                              client.copySyncConfig(config, result);
                          });

        return result.get();
    }

    bool login(const string& user, const string& pw)
    {
        future<bool> p;
        p = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); });
        if (!waitonresults(&p)) return false;
        p = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); });
        return waitonresults(&p);
    }

    bool login_fetchnodes(const string& user, const string& pw, bool makeBaseFolder = false, bool noCache = false)
    {
        future<bool> p2;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(noCache, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([makeBaseFolder](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(makeBaseFolder, pb); });
        if (!waitonresults(&p2)) return false;
        return true;
    }

    bool login_fetchnodes(const string& session)
    {
        future<bool> p2;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromSession(session, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(false, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(false, pb); });
        if (!waitonresults(&p2)) return false;
        return true;
    }

    //bool setupSync_mainthread(const std::string& localsyncrootfolder, const std::string& remotesyncrootfolder, handle syncid)
    //{
    //    //SyncConfig config{(fsBasePath / fs::u8path(localsyncrootfolder)).u8string(), drillchildnodebyname(gettestbasenode(), remotesyncrootfolder)->nodehandle, 0};
    //    return setupSync_mainthread(localsyncrootfolder, remotesyncrootfolder, syncid);
    //}

    handle setupSync_mainthread(const std::string& localsyncrootfolder, const std::string& remotesyncrootfolder, const bool isBackup = false)
    {
        fs::path syncdir = fsBasePath / fs::u8path(localsyncrootfolder);
        fs::create_directory(syncdir);
        auto fb = thread_do<handle>([=](StandardClient& mc, PromiseHandleSP pb)
            {
                mc.setupSync_inthread(remotesyncrootfolder, syncdir, isBackup,
                    [pb](UnifiedSync* us, const SyncError& se, error e)
                    {
                        pb->set_value(us != nullptr && !e && !se ? us->mConfig.getBackupId() : UNDEF);
                    });
            });
        return fb.get();
    }

    bool delSync_mainthread(handle backupId, bool keepCache = false)
    {
        future<bool> fb = thread_do<bool>([=](StandardClient& mc, PromiseBoolSP pb) { pb->set_value(mc.delSync_inthread(backupId, keepCache)); });
        return fb.get();
    }

    bool confirmModel_mainthread(Model::ModelNode* mnode, handle backupId, const bool ignoreDebris = false, const int confirm = CONFIRM_ALL)
    {
        future<bool> fb;
        fb = thread_do<bool>([backupId, mnode, ignoreDebris, confirm](StandardClient& sc, PromiseBoolSP pb) { pb->set_value(sc.confirmModel(backupId, mnode, confirm, ignoreDebris)); });
        return fb.get();
    }

    bool match(handle id, const Model::ModelNode* source)
    {
        if (!source) return false;

        auto result = thread_do<bool>([=](StandardClient& client, PromiseBoolSP result) {
            client.match(id, source, std::move(result));
        });

        return result.get();
    }

    void match(handle id, const Model::ModelNode* source, PromiseBoolSP result)
    {
        SyncInfo info;

        if (!syncSet(id, info))
        {
            result->set_value(false);
            return;
        }

        const auto* destination = client.nodeByHandle(info.h);
        result->set_value(destination && match(*destination, *source));
    }

    template<typename Predicate>
    bool waitFor(Predicate predicate, const std::chrono::seconds &timeout)
    {
        auto total = std::chrono::milliseconds(0);
        auto sleepIncrement = std::chrono::milliseconds(500);

        do
        {
            if (predicate(*this))
            {
                out() << "Predicate has matched!";

                return true;
            }

            out() << "Waiting for predicate to match...";

            std::this_thread::sleep_for(sleepIncrement);
            total += sleepIncrement;
        }
        while (total < timeout);

        out() << "Timed out waiting for predicate to match.";

        return false;
    }

    bool match(const Node& destination, const Model::ModelNode& source) const
    {
        list<pair<const Node*, decltype(&source)>> pending;

        pending.emplace_back(&destination, &source);

        for ( ; !pending.empty(); pending.pop_front())
        {
            const auto& dn = *pending.front().first;
            const auto& sn = *pending.front().second;

            // Nodes must have matching types.
            if (!sn.typematchesnodetype(dn.type)) return false;

            // Files require no further processing.
            if (dn.type == FILENODE) continue;

            map<string, decltype(&dn), CloudNameLess> dc;
            map<string, decltype(&sn), CloudNameLess> sc;

            // Index children for pairing.
            for (const auto* child : dn.children)
            {
                auto result = dc.emplace(child->displayname(), child);

                // For simplicity, duplicates consistute a match failure.
                if (!result.second) return false;
            }

            for (const auto& child : sn.kids)
            {
                auto result = sc.emplace(child->cloudName(), child.get());
                if (!result.second) return false;
            }

            // Pair children.
            for (const auto& s : sc)
            {
                // Skip the debris folder if it appears in the root.
                if (&sn == &source)
                {
                    if (CloudNameLess::equal(s.first, DEBRISFOLDER))
                    {
                        continue;
                    }
                }

                // Does this node have a pair in the destination?
                auto d = dc.find(s.first);

                // If not then there can be no match.
                if (d == dc.end()) return false;

                // Queue pair for more detailed matching.
                pending.emplace_back(d->second, s.second);

                // Consider the destination node paired.
                dc.erase(d);
            }

            // Can't have a match if we couldn't pair all destination nodes.
            if (!dc.empty()) return false;
        }

        return true;
    }

    bool backupOpenDrive(const fs::path& drivePath)
    {
        auto result = thread_do<bool>([=](StandardClient& client, PromiseBoolSP result) {
            client.backupOpenDrive(drivePath, std::move(result));
        });

        return result.get();
    }

    void backupOpenDrive(const fs::path& drivePath, PromiseBoolSP result)
    {
        auto localDrivePath = LocalPath::fromPath(drivePath.u8string(), *client.fsaccess);
        result->set_value(client.syncs.backupOpenDrive(localDrivePath) == API_OK);
    }
};


void waitonsyncs(chrono::seconds d = std::chrono::seconds(4), StandardClient* c1 = nullptr, StandardClient* c2 = nullptr, StandardClient* c3 = nullptr, StandardClient* c4 = nullptr)
{
    auto totalTimeoutStart = chrono::steady_clock::now();
    auto start = chrono::steady_clock::now();
    std::vector<StandardClient*> v{ c1, c2, c3, c4 };
    bool onelastsyncdown = true;
    for (;;)
    {
        bool any_add_del = false;

        for (auto vn : v)
        {
            if (vn)
            {
                auto result =
                  vn->thread_do<bool>(
                    [&](StandardClient& mc, PromiseBoolSP result)
                    {
                        bool busy = false;

                        mc.client.syncs.forEachRunningSync(
                          [&](Sync* s)
                          {
                              busy |= !s->deleteq.empty();
                              busy |= !s->insertq.empty();
                          });

                        if (!(mc.client.todebris.empty()
                            && mc.client.localsyncnotseen.empty()
                            && mc.client.tounlink.empty()
                            && mc.client.synccreate.empty()
                            && mc.client.transferlist.transfers[GET].empty()
                            && mc.client.transferlist.transfers[PUT].empty()))
                        {
                            busy = true;
                        }

                        result->set_value(busy);
                    });

                any_add_del |= result.get();
            }
        }

        bool allactive = true;
        {
            //lock_guard<mutex> g(StandardClient::om);
            //out() << "sync state: ";
            //for (auto n : syncstates)
            //{
            //    cout << n;
            //    if (n != SYNC_ACTIVE) allactive = false;
            //}
            //out();
        }

        if (any_add_del || StandardClient::debugging)
        {
            start = chrono::steady_clock::now();
        }

        if (onelastsyncdown && (chrono::steady_clock::now() - start + d/2) > d)
        {
            // synced folders that were removed remotely don't have the corresponding local folder removed unless we prompt an extra syncdown.  // todo:  do we need to fix
            for (auto vn : v) if (vn) vn->client.syncdownrequired = true;
            onelastsyncdown = false;
        }

        for (auto vn : v) if (vn)
        {
            if (allactive && ((chrono::steady_clock::now() - start) > d) && ((chrono::steady_clock::now() - vn->lastcb) > d))
            {
                return;
            }
        }

        WaitMillisec(400);

        if ((chrono::steady_clock::now() - totalTimeoutStart) > std::chrono::minutes(5))
        {
            out() << "Waiting for syncing to stop timed out at 5 minutes";
            return;
        }
    }

}


mutex StandardClient::om;
bool StandardClient::debugging = false;



//std::atomic<int> fileSizeCount = 20;

bool createNameFile(const fs::path &p, const string &filename)
{
    return createFile(p / fs::u8path(filename), filename.data(), filename.size());
}

bool createDataFileWithTimestamp(const fs::path &path,
                             const std::string &data,
                             const fs::file_time_type &timestamp)
{
    const bool result = createDataFile(path, data);

    if (result)
    {
        fs::last_write_time(path, timestamp);
    }

    return result;
}

bool buildLocalFolders(fs::path targetfolder, const string& prefix, int n, int recurselevel, int filesperfolder)
{
    if (suppressfiles) filesperfolder = 0;

    fs::path p = targetfolder / fs::u8path(prefix);
    if (!fs::create_directory(p))
        return false;

    for (int i = 0; i < filesperfolder; ++i)
    {
        string filename = "file" + to_string(i) + "_" + prefix;
        createNameFile(p, filename);
        //int thisSize = (++fileSizeCount)/2;
        //for (int j = 0; j < thisSize; ++j) fs << ('0' + j % 10);
    }

    if (recurselevel > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!buildLocalFolders(p, prefix + "_" + to_string(i), n, recurselevel - 1, filesperfolder))
                return false;
        }
    }

    return true;
}

void renameLocalFolders(fs::path targetfolder, const string& newprefix)
{
    std::list<fs::path> toRename;
    for (fs::directory_iterator i(targetfolder); i != fs::directory_iterator(); ++i)
    {
        if (fs::is_directory(i->path()))
        {
            renameLocalFolders(i->path(), newprefix);
        }
        toRename.push_back(i->path());
    }

    for (auto p : toRename)
    {
        auto newpath = p.parent_path() / (newprefix + p.filename().u8string());
        fs::rename(p, newpath);
    }
}


#ifdef __linux__
bool createSpecialFiles(fs::path targetfolder, const string& prefix, int n = 1)
{
    fs::path p = targetfolder;
    for (int i = 0; i < n; ++i)
    {
        string filename = "file" + to_string(i) + "_" + prefix;
        fs::path fp = p / fs::u8path(filename);

        int fdtmp = openat(AT_FDCWD, p.c_str(), O_RDWR|O_CLOEXEC|O_TMPFILE, 0600);
        write(fdtmp, filename.data(), filename.size());

        stringstream fdproc;
        fdproc << "/proc/self/fd/";
        fdproc << fdtmp;

        int r = linkat(AT_FDCWD, fdproc.str().c_str() , AT_FDCWD, fp.c_str(), AT_SYMLINK_FOLLOW);
        if (r)
        {
            cerr << " errno =" << errno;
            return false;
        }
        close(fdtmp);
    }
    return true;
}
#endif

} // anonymous

class SyncFingerprintCollision
  : public ::testing::Test
{
public:
    SyncFingerprintCollision()
      : client0()
      , client1()
      , model0()
      , model1()
      , arbitraryFileLength(16384)
    {
        const fs::path root = makeNewTestRoot();

        client0 = ::mega::make_unique<StandardClient>(root, "c0");
        client1 = ::mega::make_unique<StandardClient>(root, "c1");

        client0->logcb = true;
        client1->logcb = true;
    }

    ~SyncFingerprintCollision()
    {
    }

    void SetUp() override
    {
        SimpleLogger::setLogLevel(logMax);

        ASSERT_TRUE(client0->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "d", 1, 2));
        ASSERT_TRUE(client1->login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
        ASSERT_EQ(client0->basefolderhandle, client1->basefolderhandle);

        model0.root->addkid(model0.buildModelSubdirs("d", 2, 1, 0));
        model1.root->addkid(model1.buildModelSubdirs("d", 2, 1, 0));

        startSyncs();
        waitOnSyncs();
        confirmModels();
    }

    void addModelFile(Model &model,
                      const std::string &directory,
                      const std::string &file,
                      const std::string &content)
    {
        auto *node = model.findnode(directory);
        ASSERT_NE(node, nullptr);

        node->addkid(model.makeModelSubfile(file, content));
    }

    void confirmModel(StandardClient &client, Model &model, handle backupId)
    {
        ASSERT_TRUE(client.confirmModel_mainthread(model.findnode("d"), backupId));
    }

    void confirmModels()
    {
        confirmModel(*client0, model0, backupId0);
        confirmModel(*client1, model1, backupId1);
    }

    const fs::path localRoot0() const
    {
        return client0->syncSet(backupId0).localpath;
    }

    const fs::path localRoot1() const
    {
        return client1->syncSet(backupId1).localpath;
    }

    void startSyncs()
    {
        backupId0 = client0->setupSync_mainthread("s0", "d");
        ASSERT_NE(backupId0, UNDEF);
        backupId1 = client1->setupSync_mainthread("s1", "d");
        ASSERT_NE(backupId1, UNDEF);
    }

    void waitOnSyncs()
    {
        waitonsyncs(chrono::seconds(4), client0.get(), client1.get());
    }

    handle backupId0 = UNDEF;
    handle backupId1 = UNDEF;

    std::unique_ptr<StandardClient> client0;
    std::unique_ptr<StandardClient> client1;
    Model model0;
    Model model1;
    const std::size_t arbitraryFileLength;
}; /* SyncFingerprintCollision */

TEST_F(SyncFingerprintCollision, DifferentMacSameName)
{
    auto data0 = randomData(arbitraryFileLength);
    auto data1 = data0;
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_1" / "a";

    // Alter MAC but leave fingerprint untouched.
    data1[0x41] = static_cast<uint8_t>(~data1[0x41]);

    ASSERT_TRUE(createDataFile(path0, data0));
    waitOnSyncs();

    auto result0 =
      client0->thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
                         {
                             p->set_value(
                                 createDataFileWithTimestamp(
                                 path1,
                                 data1,
                                 fs::last_write_time(path0)));
                         });

    ASSERT_TRUE(waitonresults(&result0));
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_1", "a", data1);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_1", "a", data0);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

TEST_F(SyncFingerprintCollision, DifferentMacDifferentName)
{
    auto data0 = randomData(arbitraryFileLength);
    auto data1 = data0;
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_0" / "b";

    data1[0x41] = static_cast<uint8_t>(~data1[0x41]);

    ASSERT_TRUE(createDataFile(path0, data0));
    waitOnSyncs();

    auto result0 =
      client0->thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
                         {
                             p->set_value(
                                 createDataFileWithTimestamp(
                                 path1,
                                 data1,
                                 fs::last_write_time(path0)));
                         });

    ASSERT_TRUE(waitonresults(&result0));
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_0", "b", data1);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_0", "b", data1);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

TEST_F(SyncFingerprintCollision, SameMacDifferentName)
{
    auto data0 = randomData(arbitraryFileLength);
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_0" / "b";

    ASSERT_TRUE(createDataFile(path0, data0));
    waitOnSyncs();

    auto result0 =
      client0->thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
                         {
                            p->set_value(
                                 createDataFileWithTimestamp(
                                 path1,
                                 data0,
                                 fs::last_write_time(path0)));
                         });

    ASSERT_TRUE(waitonresults(&result0));
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_0", "b", data0);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_0", "b", data0);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

class SyncTest
    : public ::testing::Test
{
public:

    // Sets up the test fixture.
    void SetUp() override
    {
        LOG_info << "____TEST SetUp: " << ::testing::UnitTest::GetInstance()->current_test_info()->name();

        SimpleLogger::setLogLevel(logMax);
    }

    // Tears down the test fixture.
    void TearDown() override
    {
        LOG_info << "____TEST TearDown: " << ::testing::UnitTest::GetInstance()->current_test_info()->name();
    }

}; // SqliteDBTest

TEST_F(SyncTest, BasicSync_DelRemoteFolder)
{
    // delete a remote folder and confirm the client sending the request and another also synced both correctly update the disk
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2


    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // delete something remotely and let sync catch up
    future<bool> fb = clientA1.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremote("f/f_2/f_2_1", pb); });
    ASSERT_TRUE(waitonresults(&fb));
    waitonsyncs(std::chrono::seconds(60), &clientA1, &clientA2);

    // check everything matches in both syncs (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_DelLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    auto checkpath = clientA1.syncSet(backupId1).localpath.u8string();
    out() << "checking paths " << checkpath;
    for(auto& p: fs::recursive_directory_iterator(TestFS::GetTestFolder()))
    {
        out() << "checking path is present: " << p.path().u8string();
    }
    // delete something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code e;
    auto nRemoved = fs::remove_all(clientA1.syncSet(backupId1).localpath / "f_2" / "f_2_1", e);
    ASSERT_TRUE(!e) << "remove failed " << (clientA1.syncSet(backupId1).localpath / "f_2" / "f_2_1").u8string() << " error " << e;
    ASSERT_GT(nRemoved, 0) << e;

    // let them catch up
    waitonsyncs(std::chrono::seconds(20), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
}

TEST_F(SyncTest, BasicSync_MoveLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_2" / "f_2_1", clientA1.syncSet(backupId1).localpath / "f_2_1", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movenode("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_MoveLocalFolderBetweenSyncs)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2
    StandardClient clientA3(localtestroot, "clientA3");   // user 1 client 3

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA3.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1 and A2, it should build matching local folders
    handle backupId11 = clientA1.setupSync_mainthread("sync1", "f/f_0");
    ASSERT_NE(backupId11, UNDEF);
    handle backupId12 = clientA1.setupSync_mainthread("sync2", "f/f_2");
    ASSERT_NE(backupId12, UNDEF);
    handle backupId21 = clientA2.setupSync_mainthread("syncA2_1", "f/f_0");
    ASSERT_NE(backupId21, UNDEF);
    handle backupId22 = clientA2.setupSync_mainthread("syncA2_2", "f/f_2");
    ASSERT_NE(backupId22, UNDEF);
    handle backupId31 = clientA3.setupSync_mainthread("syncA3", "f");
    ASSERT_NE(backupId31, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2, &clientA3);
    clientA1.logcb = clientA2.logcb = clientA3.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_0"), backupId11));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_2"), backupId12));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_0"), backupId21));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_2"), backupId22));
    ASSERT_TRUE(clientA3.confirmModel_mainthread(model.findnode("f"), backupId31));

    // move a folder form one local synced folder to another local synced folder and see if we sync correctly and catch up in A2 and A3 (mover and observer syncs)
    error_code rename_error;
    fs::path path1 = clientA1.syncSet(backupId11).localpath / "f_0_1";
    fs::path path2 = clientA1.syncSet(backupId12).localpath / "f_2_1" / "f_2_1_0" / "f_0_1";
    fs::rename(path1, path2, rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2, &clientA3);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movenode("f/f_0/f_0_1", "f/f_2/f_2_1/f_2_1_0"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_0"), backupId11));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_2"), backupId12));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_0"), backupId21));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_2"), backupId22));
    ASSERT_TRUE(clientA3.confirmModel_mainthread(model.findnode("f"), backupId31));
}

TEST_F(SyncTest, BasicSync_RenameLocalFile)
{
    static auto TIMEOUT = std::chrono::seconds(4);

    const fs::path root = makeNewTestRoot();

    // Primary client.
    StandardClient client0(root, "c0");
    // Observer.
    StandardClient client1(root, "c1");

    // Log callbacks.
    client0.logcb = true;
    client1.logcb = true;

    // Log clients in.
    ASSERT_TRUE(client0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));
    ASSERT_TRUE(client1.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(client0.basefolderhandle, client1.basefolderhandle);

    // Set up syncs.
    handle backupId0 = client0.setupSync_mainthread("s0", "x");
    ASSERT_NE(backupId0, UNDEF);
    handle backupId1 = client1.setupSync_mainthread("s1", "x");
    ASSERT_NE(backupId1, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &client0, &client1);

    // Add x/f.
    ASSERT_TRUE(createNameFile(client0.syncSet(backupId0).localpath, "f"));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &client0, &client1);

    // Confirm model.
    Model model;

    model.root->addkid(model.makeModelSubfolder("x"));
    model.findnode("x")->addkid(model.makeModelSubfile("f"));

    ASSERT_TRUE(client0.confirmModel_mainthread(model.findnode("x"), backupId0));
    ASSERT_TRUE(client1.confirmModel_mainthread(model.findnode("x"), backupId1, true));

    // Rename x/f to x/g.
    fs::rename(client0.syncSet(backupId0).localpath / "f",
               client0.syncSet(backupId0).localpath / "g");

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &client0, &client1);

    // Update and confirm model.
    model.findnode("x/f")->name = "g";

    ASSERT_TRUE(client0.confirmModel_mainthread(model.findnode("x"), backupId0));
    ASSERT_TRUE(client1.confirmModel_mainthread(model.findnode("x"), backupId1, true));
}

TEST_F(SyncTest, BasicSync_AddLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folders (and files) in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath / "f_2", "newkid", 2, 2, 2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);  // two minutes should be long enough to get past API_ETEMPUNAVAIL == -18 for sync2 downloading the files uploaded by sync1

    // check everything matches (model has expected state of remote and local)
    model.findnode("f/f_2")->addkid(model.buildModelSubdirs("newkid", 2, 2, 2));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}


// todo: add this test once the sync can keep up with file system notifications - at the moment
// it's too slow because we wait for the cloud before processing the next layer of files+folders.
// So if we add enough changes to exercise the notification queue, we can't check the results because
// it's far too slow at the syncing stage.
TEST_F(SyncTest, BasicSync_MassNotifyFromLocalFolderTree)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    //StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 0, 0));
    //ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    //ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    //ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1/*, &clientA2*/);
    //clientA1.logcb = clientA2.logcb = true;

    // Create a directory tree in one sync, it should be synced to the cloud and back to the other
    // Create enough files and folders that we put a strain on the notification logic: 3k entries
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "initial", 0, 0, 16000));

    //waitonsyncs(std::chrono::seconds(10), &clientA1 /*, &clientA2*/);
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // wait until the notify queues subside, it shouldn't take too long.  Limit of 5 minutes
    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(5 * 60))
    {
        size_t remaining = 0;
        auto result0 = clientA1.thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
        {
            sc.client.syncs.forEachRunningSync(
              [&](Sync* s)
              {
                  for (int q = DirNotify::NUMQUEUES; q--; )
                  {
                      remaining += s->dirnotify->notifyq[q].size();
                  }
              });

            p->set_value(true);
        });
        result0.get();
        if (!remaining) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    Model model;
    model.root->addkid(model.buildModelSubdirs("initial", 0, 0, 16000));

    // check everything matches (just local since it'll still be uploading files)
    clientA1.localNodesMustHaveNodes = false;
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), backupId1, false, StandardClient::CONFIRM_LOCAL));
    //ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    ASSERT_GT(clientA1.transfersAdded.load(), 0u);
    clientA1.transfersAdded = 0;

    // rename all those files and folders, put a strain on the notify system again.
    // Also, no downloads (or uploads) should occur as a result of this.
 //   renameLocalFolders(clientA1.syncSet(backupId1).localpath, "renamed_");

    // let them catch up
    //waitonsyncs(std::chrono::seconds(10), &clientA1 /*, &clientA2*/);

    // rename is too slow to check, even just in localnodes, for now.

    //ASSERT_EQ(clientA1.transfersAdded.load(), 0u);

    //Model model2;
    //model2.root->addkid(model.buildModelSubdirs("renamed_initial", 0, 0, 100));

    //// check everything matches (model has expected state of remote and local)
    //ASSERT_TRUE(clientA1.confirmModel_mainthread(model2.root.get(), 1));
    ////ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), 2));
}



/* this one is too slow for regular testing with the current algorithm
TEST_F(SyncTest, BasicSync_MAX_NEWNODES1)
{
    // create more nodes than we can upload in one putnodes.
    // this tree is 5x5 and the algorithm ends up creating nodes one at a time so it's pretty slow (and doesn't hit MAX_NEWNODES as a result)
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "f", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    // make new folders in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    assert(MegaClient::MAX_NEWNODES < 3125);
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "g", 5, 5, 0));  // 5^5=3125 leaf folders, 625 pre-leaf etc

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("g", 5, 5, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));
}
*/

/* this one is too slow for regular testing with the current algorithm
TEST_F(SyncTest, BasicSync_MAX_NEWNODES2)
{
    // create more nodes than we can upload in one putnodes.
    // this tree is 5x5 and the algorithm ends up creating nodes one at a time so it's pretty slow (and doesn't hit MAX_NEWNODES as a result)
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "f", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    // make new folders in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    assert(MegaClient::MAX_NEWNODES < 3000);
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "g", 3000, 1, 0));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("g", 3000, 1, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));
}
*/

TEST_F(SyncTest, BasicSync_MoveExistingIntoNewLocalFolder)
{
    // historic case:  in the local filesystem, create a new folder then move an existing file/folder into it
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folder in the local filesystem
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "new", 1, 0, 0));
    // move an already synced folder into it
    error_code rename_error;
    fs::path path1 = clientA1.syncSet(backupId1).localpath / "f_2"; // / "f_2_0" / "f_2_0_0";
    fs::path path2 = clientA1.syncSet(backupId1).localpath / "new" / "f_2"; // "f_2_0_0";
    fs::rename(path1, path2, rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(10), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    auto f = model.makeModelSubfolder("new");
    f->addkid(model.removenode("f/f_2")); // / f_2_0 / f_2_0_0"));
    model.findnode("f")->addkid(move(f));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_MoveSeveralExistingIntoDeepNewLocalFolders)
{
    // historic case:  in the local filesystem, create a new folder then move an existing file/folder into it
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folder tree in the local filesystem
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "new", 3, 3, 3));

    // move already synced folders to serveral parts of it - one under another moved folder too
    error_code rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "new" / "new_0" / "new_0_1" / "new_0_1_2" / "f_0", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_1", clientA1.syncSet(backupId1).localpath / "new" / "new_1" / "new_1_2" / "f_1", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_2", clientA1.syncSet(backupId1).localpath / "new" / "new_1" / "new_1_2" / "f_1" / "f_1_2" / "f_2", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("new", 3, 3, 3));
    model.findnode("f/new/new_0/new_0_1/new_0_1_2")->addkid(model.removenode("f/f_0"));
    model.findnode("f/new/new_1/new_1_2")->addkid(model.removenode("f/f_1"));
    model.findnode("f/new/new_1/new_1_2/f_1/f_1_2")->addkid(model.removenode("f/f_2"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

/* not expected to work yet
TEST_F(SyncTest, BasicSync_SyncDuplicateNames)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);


    NewNode* nodearray = new NewNode[3];
    nodearray[0] = *clientA1.makeSubfolder("samename");
    nodearray[1] = *clientA1.makeSubfolder("samename");
    nodearray[2] = *clientA1.makeSubfolder("Samename");
    clientA1.resultproc.prepresult(StandardClient::PUTNODES, [this](error e) {
    });
    clientA1.client.putnodes(clientA1.basefolderhandle, nodearray, 3);

    // set up syncs, they should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.makeModelSubfolder("samename"));
    model.root->addkid(model.makeModelSubfolder("samename"));
    model.root->addkid(model.makeModelSubfolder("Samename"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/

TEST_F(SyncTest, BasicSync_RemoveLocalNodeBeforeSessionResume)
{
    fs::path localtestroot = makeNewTestRoot();
    auto pclientA1 = ::mega::make_unique<StandardClient>(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // save session
    string session;
    pclientA1->client.dumpsession(session);

    // logout (but keep caches)
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;
    pclientA1->localLogout();

    // remove local folders
    error_code e;
    ASSERT_TRUE(fs::remove_all(sync1path / "f_2", e) != static_cast<std::uintmax_t>(-1)) << e;

    // resume session, see if nodes and localnodes get in sync
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));

    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2", "f"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
}

/* not expected to work yet
TEST_F(SyncTest, BasicSync_RemoteFolderCreationRaceSamename)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    // SN tagging needed for this one
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for both, it should build matching local folders (empty initially)
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // now have both clients create the same remote folder structure simultaneously.  We should end up with just one copy of it on the server and in both syncs
    future<bool> p1 = clientA1.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("f", 3, 3, pb); });
    future<bool> p2 = clientA2.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("f", 3, 3, pb); });
    ASSERT_TRUE(waitonresults(&p1, &p2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/

/* not expected to work yet
TEST_F(SyncTest, BasicSync_LocalFolderCreationRaceSamename)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    // SN tagging needed for this one
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for both, it should build matching local folders (empty initially)
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // now have both clients create the same folder structure simultaneously.  We should end up with just one copy of it on the server and in both syncs
    future<bool> p1 = clientA1.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { buildLocalFolders(sc.syncSet(backupId1).localpath, "f", 3, 3, 0); pb->set_value(true); });
    future<bool> p2 = clientA2.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { buildLocalFolders(sc.syncSet(backupId2).localpath, "f", 3, 3, 0); pb->set_value(true); });
    ASSERT_TRUE(waitonresults(&p1, &p2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/


TEST_F(SyncTest, BasicSync_ResumeSyncFromSessionAfterNonclashingLocalAndRemoteChanges )
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model1, model2;
    model1.root->addkid(model1.buildModelSubdirs("f", 3, 3, 0));
    model2.root->addkid(model2.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), backupId2));

    out() << "********************* save session A1";
    string session;
    pclientA1->client.dumpsession(session);

    out() << "*********************  logout A1 (but keep caches on disk)";
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;
    pclientA1->localLogout();

    out() << "*********************  add remote folders via A2";
    future<bool> p1 = clientA2.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("newremote", 2, 2, pb, "f/f_1/f_1_0"); });
    model1.findnode("f/f_1/f_1_0")->addkid(model1.buildModelSubdirs("newremote", 2, 2, 0));
    model2.findnode("f/f_1/f_1_0")->addkid(model2.buildModelSubdirs("newremote", 2, 2, 0));
    ASSERT_TRUE(waitonresults(&p1));

    out() << "*********************  remove remote folders via A2";
    p1 = clientA2.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremote("f/f_0", pb); });
    model1.movetosynctrash("f/f_0", "f");
    model2.movetosynctrash("f/f_0", "f");
    ASSERT_TRUE(waitonresults(&p1));

    out() << "*********************  add local folders in A1";
    ASSERT_TRUE(buildLocalFolders(sync1path / "f_1/f_1_2", "newlocal", 2, 2, 2));
    model1.findnode("f/f_1/f_1_2")->addkid(model1.buildModelSubdirs("newlocal", 2, 2, 2));
    model2.findnode("f/f_1/f_1_2")->addkid(model2.buildModelSubdirs("newlocal", 2, 2, 2));

    out() << "*********************  remove local folders in A1";
    error_code e;
    ASSERT_TRUE(fs::remove_all(sync1path / "f_2", e) != static_cast<std::uintmax_t>(-1)) << e;
    model1.removenode("f/f_2");
    model2.movetosynctrash("f/f_2", "f");

    out() << "*********************  get sync2 activity out of the way";
    waitonsyncs(DEFAULTWAIT, &clientA2);

    out() << "*********************  resume A1 session (with sync), see if A2 nodes and localnodes get in sync again";
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);
    waitonsyncs(DEFAULTWAIT, pclientA1.get(), &clientA2);

    out() << "*********************  check everything matches (model has expected state of remote and local)";
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    model2.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_ResumeSyncFromSessionAfterClashingLocalAddRemoteDelete)
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // save session A1
    string session;
    pclientA1->client.dumpsession(session);
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;

    // logout A1 (but keep caches on disk)
    pclientA1->localLogout();

    // remove remote folder via A2
    future<bool> p1 = clientA2.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremote("f/f_1", pb); });
    ASSERT_TRUE(waitonresults(&p1));

    // add local folders in A1 on disk folder
    ASSERT_TRUE(buildLocalFolders(sync1path / "f_1/f_1_2", "newlocal", 2, 2, 2));

    // get sync2 activity out of the way
    waitonsyncs(std::chrono::seconds(4), &clientA2);

    // resume A1 session (with sync), see if A2 nodes and localnodes get in sync again
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);
    waitonsyncs(std::chrono::seconds(10), pclientA1.get(), &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f/f_1/f_1_2")->addkid(model.buildModelSubdirs("newlocal", 2, 2, 2));
    ASSERT_TRUE(model.movetosynctrash("f/f_1", "f"));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(model.removesynctrash("f", "f_1/f_1_2/newlocal"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}


TEST_F(SyncTest, CmdChecks_RRAttributeAfterMoveNode)
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));

    Node* f = pclientA1->drillchildnodebyname(pclientA1->gettestbasenode(), "f");
    handle original_f_handle = f->nodehandle;
    handle original_f_parent_handle = f->parent->nodehandle;

    // make sure there are no 'f' in the rubbish
    auto fv = pclientA1->drillchildnodesbyname(pclientA1->getcloudrubbishnode(), "f");
    future<bool> fb = pclientA1->thread_do<bool>([&fv](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremotenodes(fv, pb); });
    ASSERT_TRUE(waitonresults(&fb));

    f = pclientA1->drillchildnodebyname(pclientA1->getcloudrubbishnode(), "f");
    ASSERT_TRUE(f == nullptr);


    // remove remote folder via A2
    future<bool> p1 = pclientA1->thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb)
        {
            sc.movenodetotrash("f", pb);
        });
    ASSERT_TRUE(waitonresults(&p1));

    WaitMillisec(3000);  // allow for attribute delivery too

    f = pclientA1->drillchildnodebyname(pclientA1->getcloudrubbishnode(), "f");
    ASSERT_TRUE(f != nullptr);

    // check the restore-from-trash handle got set, and correctly
    nameid rrname = AttrMap::string2nameid("rr");
    ASSERT_EQ(f->nodehandle, original_f_handle);
    ASSERT_EQ(f->attrs.map[rrname], string(Base64Str<MegaClient::NODEHANDLE>(original_f_parent_handle)));
    ASSERT_EQ(f->attrs.map[rrname], string(Base64Str<MegaClient::NODEHANDLE>(pclientA1->gettestbasenode()->nodehandle)));

    // move it back

    p1 = pclientA1->thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb)
    {
        sc.movenode(f->nodehandle, pclientA1->basefolderhandle, pb);
    });
    ASSERT_TRUE(waitonresults(&p1));

    WaitMillisec(3000);  // allow for attribute delivery too

    // check it's back and the rr attribute is gone
    f = pclientA1->drillchildnodebyname(pclientA1->gettestbasenode(), "f");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ(f->attrs.map[rrname], string());
}


#ifdef __linux__
TEST_F(SyncTest, BasicSync_SpecialCreateFile)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 2, 2));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 2, 2, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folders (and files) in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    ASSERT_TRUE(createSpecialFiles(clientA1.syncSet(backupId1).localpath / "f_0", "newkid", 2));

    for (int i = 0; i < 2; ++i)
    {
        string filename = "file" + to_string(i) + "_" + "newkid";
        model.findnode("f/f_0")->addkid(model.makeModelSubfile(filename));
    }

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}
#endif

TEST_F(SyncTest, DISABLED_BasicSync_moveAndDeleteLocalFile)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "renamed", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::remove(clientA1.syncSet(backupId1).localpath / "renamed");

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_0", "f"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
}

namespace {

string makefa(const string& name, int fakecrc, int mtime)
{
    AttrMap attrs;
    attrs.map['n'] = name;

    FileFingerprint ff;
    ff.crc[0] = ff.crc[1] = ff.crc[2] = ff.crc[3] = fakecrc;
    ff.mtime = mtime;
    ff.serializefingerprint(&attrs.map['c']);

    string attrjson;
    attrs.getjson(&attrjson);
    return attrjson;
}

Node* makenode(MegaClient& mc, handle parent, ::mega::nodetype_t type, m_off_t size, handle owner, const string& attrs, ::mega::byte* key)
{
    static handle handlegenerator = 10;
    std::vector<Node*> dp;
    auto newnode = new Node(&mc, &dp, ++handlegenerator, parent, type, size, owner, nullptr, 1);

    newnode->setkey(key);
    newnode->attrstring.reset(new string);

    SymmCipher sc;
    sc.setkey(key, type);
    mc.makeattr(&sc, newnode->attrstring, attrs.c_str());

    int attrlen = int(newnode->attrstring->size());
    string base64attrstring;
    base64attrstring.resize(static_cast<size_t>(attrlen * 4 / 3 + 4));
    base64attrstring.resize(static_cast<size_t>(Base64::btoa((::mega::byte *)newnode->attrstring->data(), int(newnode->attrstring->size()), (char *)base64attrstring.data())));

    *newnode->attrstring = base64attrstring;

    return newnode;
}

} // anonymous

TEST_F(SyncTest, NodeSorting_forPhotosAndVideos)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient standardclient(localtestroot, "sortOrderTests");
    auto& client = standardclient.client;

    handle owner = 99999;

    ::mega::byte key[] = { 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04 };

    // first 3 are root nodes:
    auto cloudroot = makenode(client, UNDEF, ROOTNODE, -1, owner, makefa("root", 1, 1), key);
    makenode(client, UNDEF, INCOMINGNODE, -1, owner, makefa("inbox", 1, 1), key);
    makenode(client, UNDEF, RUBBISHNODE, -1, owner, makefa("bin", 1, 1), key);

    // now some files to sort
    auto photo1 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("abc.jpg", 1, 1570673890), key);
    auto photo2 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("cba.png", 1, 1570673891), key);
    auto video1 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("xyz.mov", 1, 1570673892), key);
    auto video2 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("zyx.mp4", 1, 1570673893), key);
    auto otherfile = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("ASDF.fsda", 1, 1570673894), key);
    auto otherfolder = makenode(client, cloudroot->nodehandle, FOLDERNODE, -1, owner, makefa("myfolder", 1, 1570673895), key);

    node_vector v{ photo1, photo2, video1, video2, otherfolder, otherfile };
    for (auto n : v) n->setkey(key);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_PHOTO_ASC, client);
    node_vector v2{ photo1, photo2, video1, video2, otherfolder, otherfile };
    ASSERT_EQ(v, v2);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_PHOTO_DESC, client);
    node_vector v3{ photo2, photo1, video2, video1, otherfolder, otherfile };
    ASSERT_EQ(v, v3);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_VIDEO_ASC, client);
    node_vector v4{ video1, video2, photo1, photo2, otherfolder, otherfile };
    ASSERT_EQ(v, v4);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_VIDEO_DESC, client);
    node_vector v5{ video2, video1, photo2, photo1, otherfolder, otherfile };
    ASSERT_EQ(v, v5);
}


TEST_F(SyncTest, PutnodesForMultipleFolders)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient standardclient(localtestroot, "PutnodesForMultipleFolders");
    ASSERT_TRUE(standardclient.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", true));

    vector<NewNode> newnodes(4);

    standardclient.client.putnodes_prepareOneFolder(&newnodes[0], "folder1");
    standardclient.client.putnodes_prepareOneFolder(&newnodes[1], "folder2");
    standardclient.client.putnodes_prepareOneFolder(&newnodes[2], "folder2.1");
    standardclient.client.putnodes_prepareOneFolder(&newnodes[3], "folder2.2");

    newnodes[1].nodehandle = newnodes[2].parenthandle = newnodes[3].parenthandle = 2;

    auto targethandle = NodeHandle().set6byte(standardclient.client.rootnodes[0]);

    std::atomic<bool> putnodesDone{false};
    standardclient.resultproc.prepresult(StandardClient::PUTNODES,  ++next_request_tag,
        [&](){ standardclient.client.putnodes(targethandle, move(newnodes), nullptr, standardclient.client.reqtag); },
        [&putnodesDone](error e) { putnodesDone = true; return true; });

    while (!putnodesDone)
    {
        WaitMillisec(100);
    }

    Node* cloudRoot = standardclient.client.nodeByHandle(targethandle);

    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder1"));
    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder2"));
    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder2/folder2.1"));
    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder2/folder2.2"));
}

TEST_F(SyncTest, ExerciseCommands)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient standardclient(localtestroot, "ExerciseCommands");
    ASSERT_TRUE(standardclient.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", true));

    // Using this set setup to execute commands direct in the SDK Core
    // so that we can test things that the MegaApi interface would
    // disallow or shortcut.

    // make sure it's a brand new folder
    future<bool> p1 = standardclient.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("testlinkfolder_brandnew3", 1, 1, pb); });
    ASSERT_TRUE(waitonresults(&p1));

    assert(standardclient.lastPutnodesResultFirstHandle != UNDEF);
    Node* n2 = standardclient.client.nodebyhandle(standardclient.lastPutnodesResultFirstHandle);

    out() << "Testing make public link for node: " << n2->displaypath();

    // try to get a link on an existing unshared folder
    promise<Error> pe1, pe1a, pe2, pe3, pe4;
    standardclient.getpubliclink(n2, 0, 0, false, pe1);
    ASSERT_EQ(API_EACCESS, pe1.get_future().get());

    // create on existing node
    standardclient.exportnode(n2, 0, 0, false, pe1a);
    ASSERT_EQ(API_OK, pe1a.get_future().get());

    // get link on existing shared folder node, with link already  (different command response)
    standardclient.getpubliclink(n2, 0, 0, false, pe2);
    ASSERT_EQ(API_OK, pe2.get_future().get());

    // delete existing link on node
    standardclient.getpubliclink(n2, 1, 0, false, pe3);
    ASSERT_EQ(API_OK, pe3.get_future().get());

    // create on non existent node
    n2->nodehandle = UNDEF;
    standardclient.getpubliclink(n2, 0, 0, false, pe4);
    ASSERT_EQ(API_EACCESS, pe4.get_future().get());
}

#ifndef _WIN32_SUPPORTS_SYMLINKS_IT_JUST_NEEDS_TURNING_ON
TEST_F(SyncTest, BasicSync_CreateAndDeleteLink)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    fs::remove(clientA1.syncSet(backupId1).localpath / "linked");
    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_CreateRenameAndDeleteLink)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::rename(clientA1.syncSet(backupId1).localpath / "linked", clientA1.syncSet(backupId1).localpath / "linkrenamed", linkage_error);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::remove(clientA1.syncSet(backupId1).localpath / "linkrenamed");

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

#ifndef WIN32

// what is supposed to happen for this one?  It seems that the `linked` symlink is no longer ignored on windows?  client2 is affected!

TEST_F(SyncTest, BasicSync_CreateAndReplaceLinkLocally)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::remove(clientA1.syncSet(backupId1).localpath / "linked");
    ASSERT_TRUE(createNameFile(clientA1.syncSet(backupId1).localpath, "linked"));

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    model.findnode("f")->addkid(model.makeModelSubfile("linked"));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files

    //check client 2 is as expected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}


TEST_F(SyncTest, BasicSync_CreateAndReplaceLinkUponSyncDown)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    ASSERT_TRUE(createNameFile(clientA2.syncSet(backupId2).localpath, "linked"));

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    model.findnode("f")->addkid(model.makeModelSubfolder("linked")); //notice: the deleted here is folder because what's actually deleted is a symlink that points to a folder
                                                                     //ideally we could add full support for symlinks in this tests suite

    model.movetosynctrash("f/linked","f");
    model.findnode("f")->addkid(model.makeModelSubfile("linked"));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files

    //check client 2 is as expected
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
}
#endif

#endif

TEST_F(SyncTest, BasicSync_NewVersionsCreatedWhenFilesModified)
{
    // Convenience.
    using FileFingerprintPtr = unique_ptr<FileFingerprint>;

    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = std::chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Helper for generating fingerprints.
    auto fingerprint =
      [&c](const fs::path& fsPath) -> FileFingerprintPtr
      {
          // Convenience.
          auto& fsAccess = *c.client.fsaccess;

          // Needed so we can access the filesystem.
          auto fileAccess = fsAccess.newfileaccess(false);

          // Translate input path into something useful.
          auto path = LocalPath::fromPath(fsPath.u8string(), fsAccess);

          // Try and open file for reading.
          if (fileAccess->fopen(path, true, false))
          {
              auto fingerprint = ::mega::make_unique<FileFingerprint>();

              // Generate fingerprint.
              if (fingerprint->genfingerprint(fileAccess.get()))
              {
                  return fingerprint;
              }
          }

          return nullptr;
      };

    // Fingerprints for each revision.
    vector<FileFingerprintPtr> fingerprints;

    // Log client in.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));

    // Add and start sync.
    const auto id = c.setupSync_mainthread("s", "x");
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c.syncSet(id).localpath;

    // Create and populate model.
    Model model;

    model.addfile("f", "a");
    model.generate(SYNCROOT);

    // Keep track of fingerprint.
    fingerprints.emplace_back(fingerprint(SYNCROOT / "f"));
    ASSERT_TRUE(fingerprints.back());

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Check that the file made it to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Create a new revision of f.
    model.addfile("f", "b");
    model.generate(SYNCROOT);

    // Update fingerprint.
    fingerprints.emplace_back(fingerprint(SYNCROOT / "f"));
    ASSERT_TRUE(fingerprints.back());

    // Wait for change to propagate.
    waitonsyncs(TIMEOUT, &c);

    // Validate model.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Create yet anothet revision of f.
    model.addfile("f", "c");
    model.generate(SYNCROOT);

    // Update fingerprint.
    fingerprints.emplace_back(fingerprint(SYNCROOT / "f"));
    ASSERT_TRUE(fingerprints.back());

    // Wait for change to propagate.
    waitonsyncs(TIMEOUT, &c);

    // Validate model.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Get our hands on f's node.
    auto *f = c.drillchildnodebyname(c.gettestbasenode(), "x/f");
    ASSERT_TRUE(f);

    // Validate the version chain.
    auto i = fingerprints.crbegin();
    auto matched = true;

    while (f && i != fingerprints.crend())
    {
        matched &= *f == **i++;

        f = f->children.empty() ? nullptr : f->children.front();
    }

    matched &= !f && i == fingerprints.crend();
    ASSERT_TRUE(matched);
}

TEST_F(SyncTest, BasicSync_ClientToSDKConfigMigration)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = std::chrono::seconds(4);

    SyncConfig config0;
    SyncConfig config1;
    Model model;

    // Create some syncs for us to migrate.
    {
        StandardClient c0(TESTROOT, "c0");

        // Log callbacks.
        c0.logcb = true;

        // Log in client.
        ASSERT_TRUE(c0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 1, 2));

        // Add syncs.
        auto id0 = c0.setupSync_mainthread("s0", "s/s_0");
        ASSERT_NE(id0, UNDEF);

        auto id1 = c0.setupSync_mainthread("s1", "s/s_1");
        ASSERT_NE(id1, UNDEF);

        // Populate filesystem.
        auto root0 = c0.syncSet(id0).localpath;
        auto root1 = c0.syncSet(id1).localpath;

        model.addfile("d/f");
        model.addfile("f");
        model.generate(root0);
        model.generate(root1, true);

        // Wait for sync to complete.
        waitonsyncs(TIMEOUT, &c0);

        // Make sure everything arrived safely.
        ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id0));
        ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id1));

        // Get our hands on the configs.
        config0 = c0.syncConfigByBackupID(id0);
        config1 = c0.syncConfigByBackupID(id1);
    }

    // Migrate the configs.
    StandardClient c1(TESTROOT, "c1");

    // Log callbacks.
    c1.logcb = true;

    // Log in the client.
    ASSERT_TRUE(c1.login("MEGA_EMAIL", "MEGA_PWD"));

    // Make sure sync user attributes are present.
    ASSERT_TRUE(c1.ensureSyncUserAttributes());

    // Update configs so they're useful for this client.
    {
        FSACCESS_CLASS fsAccess;
        auto root0 = TESTROOT / "c1" / "s0";
        auto root1 = TESTROOT / "c1" / "s1";

        // Issue new backup IDs.
        config0.mBackupId = UNDEF;
        config1.mBackupId = UNDEF;

        // Update path for c1.
        config0.mLocalPath = LocalPath::fromPath(root0.u8string(), fsAccess);
        config1.mLocalPath = LocalPath::fromPath(root1.u8string(), fsAccess);

        // Make sure local sync roots exist.
        fs::create_directories(root0);
        fs::create_directories(root1);
    }

    // Migrate the configs.
    auto id0 = c1.copySyncConfig(config0);
    ASSERT_NE(id0, UNDEF);
    auto id1 = c1.copySyncConfig(config1);
    ASSERT_NE(id1, UNDEF);

    // Fetch nodes (and resume syncs.)
    ASSERT_TRUE(c1.fetchnodes());

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c1);

    // Check that all files from the cloud were downloaded.
    model.ensureLocalDebrisTmpLock("");
    ASSERT_TRUE(c1.confirmModel_mainthread(model.root.get(), id0));
    model.removenode(DEBRISFOLDER);
    ASSERT_TRUE(c1.confirmModel_mainthread(model.root.get(), id1));
}

/*
TEST_F(SyncTest, DetectsAndReportsNameClashes)
{
    const auto TESTFOLDER = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    StandardClient client(TESTFOLDER, "c");

    // Log in client.
    ASSERT_TRUE(client.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));

    // Needed so that we can create files with the same name.
    client.client.versions_disabled = true;

    // Populate local filesystem.
    const auto root = TESTFOLDER / "c" / "s";

    fs::create_directories(root / "d" / "e");

    createNameFile(root / "d", "f0");
    createNameFile(root / "d", "f%30");
    createNameFile(root / "d" / "e", "g0");
    createNameFile(root / "d" / "e", "g%30");

    // Start the sync.
    handle backupId1 = client.setupSync_mainthread("s", "x");
    ASSERT_NE(backupId1, UNDEF);

    // Give the client time to synchronize.
    waitonsyncs(TIMEOUT, &client);

    // Helpers.
    auto localConflictDetected = [](const NameConflict& nc, const LocalPath& name)
    {
        auto i = nc.clashingLocalNames.begin();
        auto j = nc.clashingLocalNames.end();

        return std::find(i, j, name) != j;
    };

    // Were any conflicts detected?
    ASSERT_TRUE(client.conflictsDetected());

    // Can we obtain a list of the conflicts?
    list<NameConflict> conflicts;
    ASSERT_TRUE(client.conflictsDetected(conflicts));
    ASSERT_EQ(conflicts.size(), 2u);
    ASSERT_EQ(conflicts.back().localPath, LocalPath::fromPath("d", *client.fsaccess).prependNewWithSeparator(client.syncByBackupId(backupId1)->localroot->localname));
    ASSERT_EQ(conflicts.back().clashingLocalNames.size(), 2u);
    ASSERT_TRUE(localConflictDetected(conflicts.back(), LocalPath::fromPath("f%30", *client.fsaccess)));
    ASSERT_TRUE(localConflictDetected(conflicts.back(), LocalPath::fromPath("f0", *client.fsaccess)));
    ASSERT_EQ(conflicts.back().clashingCloudNames.size(), 0u);

    // Resolve the f0 / f%30 conflict.
    ASSERT_TRUE(fs::remove(root / "d" / "f%30"));

    // Give the sync some time to think.
    waitonsyncs(TIMEOUT, &client);

    // We should still detect conflicts.
    ASSERT_TRUE(client.conflictsDetected());

    // Has the list of conflicts changed?
    conflicts.clear();
    ASSERT_TRUE(client.conflictsDetected(conflicts));
    ASSERT_GE(conflicts.size(), 1u);
    ASSERT_EQ(conflicts.front().localPath, LocalPath::fromPath("e", *client.fsaccess)
        .prependNewWithSeparator(LocalPath::fromPath("d", *client.fsaccess))
        .prependNewWithSeparator(client.syncByBackupId(backupId1)->localroot->localname));
    ASSERT_EQ(conflicts.front().clashingLocalNames.size(), 2u);
    ASSERT_TRUE(localConflictDetected(conflicts.front(), LocalPath::fromPath("g%30", *client.fsaccess)));
    ASSERT_TRUE(localConflictDetected(conflicts.front(), LocalPath::fromPath("g0", *client.fsaccess)));
    ASSERT_EQ(conflicts.front().clashingCloudNames.size(), 0u);

    // Resolve the g / g%30 conflict.
    ASSERT_TRUE(fs::remove(root / "d" / "e" / "g%30"));

    // Give the sync some time to think.
    waitonsyncs(TIMEOUT, &client);

    // No conflicts should be reported.
    ASSERT_FALSE(client.conflictsDetected());

    // Is the list of conflicts empty?
    conflicts.clear();
    ASSERT_FALSE(client.conflictsDetected(conflicts));
    ASSERT_EQ(conflicts.size(), 0u);

    // Create a remote name clash.
    auto* node = client.drillchildnodebyname(client.gettestbasenode(), "x/d");
    ASSERT_TRUE(!!node);
    ASSERT_TRUE(client.uploadFile(root / "d" / "f0", "h", node));
    ASSERT_TRUE(client.uploadFile(root / "d" / "f0", "h", node));

    // Let the client attempt to synchronize.
    waitonsyncs(TIMEOUT, &client);

    // Have we detected any conflicts?
    conflicts.clear();
    ASSERT_TRUE(client.conflictsDetected(conflicts));

    // Does our list of conflicts include remotes?
    ASSERT_GE(conflicts.size(), 1u);
    ASSERT_EQ(conflicts.front().cloudPath, string("/mega_test_sync/x/d"));
    ASSERT_EQ(conflicts.front().clashingCloudNames.size(), 2u);
    ASSERT_EQ(conflicts.front().clashingCloudNames[0], string("h"));
    ASSERT_EQ(conflicts.front().clashingCloudNames[1], string("h"));
    ASSERT_EQ(conflicts.front().clashingLocalNames.size(), 0u);

    // Resolve the remote conflict.
    ASSERT_TRUE(client.deleteremote("x/d/h"));

    // Wait for the client to process our changes.
    waitonsyncs(TIMEOUT, &client);

    conflicts.clear();
    client.conflictsDetected(conflicts);
    ASSERT_EQ(0, conflicts.size());

    // Conflicts should be resolved.
    ASSERT_FALSE(client.conflictsDetected());
}
*/

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_DoesntDownloadFilesWithClashingNames)
{
    const auto TESTFOLDER = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    // Populate cloud.
    {
        StandardClient cu(TESTFOLDER, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log client in.
        ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));

        // Needed so that we can create files with the same name.
        cu.client.versions_disabled = true;

        // Create local test hierarchy.
        const auto root = TESTFOLDER / "cu" / "x";

        // d will be duplicated and generate a clash.
        fs::create_directories(root / "d");

        // dd will be singular, no clash.
        fs::create_directories(root / "dd");

        // f will be duplicated and generate a clash.
        ASSERT_TRUE(createNameFile(root, "f"));

        // ff will be singular, no clash.
        ASSERT_TRUE(createNameFile(root, "ff"));

        auto* node = cu.drillchildnodebyname(cu.gettestbasenode(), "x");
        ASSERT_TRUE(!!node);

        // Upload d twice, generate clash.
        ASSERT_TRUE(cu.uploadFolderTree(root / "d", node));
        ASSERT_TRUE(cu.uploadFolderTree(root / "d", node));

        // Upload dd once.
        ASSERT_TRUE(cu.uploadFolderTree(root / "dd", node));

        // Upload f twice, generate clash.
        ASSERT_TRUE(cu.uploadFile(root / "f", node));
        ASSERT_TRUE(cu.uploadFile(root / "f", node));

        // Upload ff once.
        ASSERT_TRUE(cu.uploadFile(root / "ff", node));
    }

    StandardClient cd(TESTFOLDER, "cd");

    // Log callbacks.
    cd.logcb = true;

    // Log in client.
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Add and start sync.
    handle backupId1 = cd.setupSync_mainthread("sd", "x");
    ASSERT_NE(backupId1, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Populate and confirm model.
    Model model;

    // d and f are missing due to name collisions in the cloud.
    model.root->addkid(model.makeModelSubfolder("x"));
    model.findnode("x")->addkid(model.makeModelSubfolder("dd"));
    model.findnode("x")->addkid(model.makeModelSubfile("ff"));

    // Needed because we've downloaded files.
    model.ensureLocalDebrisTmpLock("x");

    // Confirm the model.
    ASSERT_TRUE(cd.confirmModel_mainthread(
                  model.findnode("x"),
                  backupId1,
                  false,
                  StandardClient::CONFIRM_LOCAL));

    // Resolve the name collisions.
    ASSERT_TRUE(cd.deleteremote("x/d"));
    ASSERT_TRUE(cd.deleteremote("x/f"));

    // Wait for the sync to update.
    waitonsyncs(TIMEOUT, &cd);

    // Confirm that d and f have now been downloaded.
    model.findnode("x")->addkid(model.makeModelSubfolder("d"));
    model.findnode("x")->addkid(model.makeModelSubfile("f"));

    // Local FS, Local Tree and Remote Tree should now be consistent.
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_DoesntUploadFilesWithClashingNames)
{
    const auto TESTFOLDER = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    // Download client.
    StandardClient cd(TESTFOLDER, "cd");
    // Upload client.
    StandardClient cu(TESTFOLDER, "cu");

    // Log callbacks.
    cd.logcb = true;
    cu.logcb = true;

    // Log in the clients.
    ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(cd.basefolderhandle, cu.basefolderhandle);

    // Populate the local filesystem.
    const auto root = TESTFOLDER / "cu" / "su";

    // Make sure clashing directories are skipped.
    fs::create_directories(root / "d0");
    fs::create_directories(root / "d%30");

    // Make sure other directories are uploaded.
    fs::create_directories(root / "d1");

    // Make sure clashing files are skipped.
    createNameFile(root, "f0");
    createNameFile(root, "f%30");

    // Make sure other files are uploaded.
    createNameFile(root, "f1");
    createNameFile(root / "d1", "f0");

    // Start the syncs.
    handle backupId1 = cd.setupSync_mainthread("sd", "x");
    handle backupId2 = cu.setupSync_mainthread("su", "x");
    ASSERT_NE(backupId1, UNDEF);
    ASSERT_NE(backupId2, UNDEF);

    // Wait for the initial sync to complete.
    waitonsyncs(TIMEOUT, &cu, &cd);

    // Populate and confirm model.
    Model model;

    model.root->addkid(model.makeModelSubfolder("root"));
    model.findnode("root")->addkid(model.makeModelSubfolder("d1"));
    model.findnode("root")->addkid(model.makeModelSubfile("f1"));
    model.findnode("root/d1")->addkid(model.makeModelSubfile("f0"));

    model.ensureLocalDebrisTmpLock("root");

    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("root"), backupId1));

    // Remove the clashing nodes.
    fs::remove_all(root / "d0");
    fs::remove_all(root / "f0");

    // Wait for the sync to complete.
    waitonsyncs(TIMEOUT, &cd, &cu);

    // Confirm that d0 and f0 have been downloaded.
    model.findnode("root")->addkid(model.makeModelSubfolder("d0"));
    model.findnode("root")->addkid(model.makeModelSubfile("f0", "f%30"));

    ASSERT_TRUE(cu.confirmModel_mainthread(model.findnode("root"), backupId2, true));
}

TEST_F(SyncTest, DISABLED_RemotesWithControlCharactersSynchronizeCorrectly)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    // Populate cloud.
    {
        // Upload client.
        StandardClient cu(TESTROOT, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log in client and clear remote contents.
        ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));

        auto* node = cu.drillchildnodebyname(cu.gettestbasenode(), "x");
        ASSERT_TRUE(!!node);

        // Create some directories containing control characters.
        vector<NewNode> nodes(2);

        // Only some platforms will escape BEL.
        cu.client.putnodes_prepareOneFolder(&nodes[0], "d\7");
        cu.client.putnodes_prepareOneFolder(&nodes[1], "d");

        ASSERT_TRUE(cu.putnodes(node->nodeHandle(), std::move(nodes)));

        // Do the same but with some files.
        auto root = TESTROOT / "cu" / "x";
        fs::create_directories(root);

        // Placeholder name.
        ASSERT_TRUE(createNameFile(root, "f"));

        // Upload files.
        ASSERT_TRUE(cu.uploadFile(root / "f", "f\7", node));
        ASSERT_TRUE(cu.uploadFile(root / "f", node));
    }

    // Download client.
    StandardClient cd(TESTROOT, "cd");

    // Log callbacks.
    cd.logcb = true;

    // Log in client.
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Add and start sync.
    handle backupId1 = cd.setupSync_mainthread("sd", "x");
    ASSERT_NE(backupId1, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Populate and confirm model.
    Model model;

    model.addfolder("x/d\7");
    model.addfolder("x/d");
    model.addfile("x/f\7", "f");
    model.addfile("x/f", "f");

    // Needed because we've downloaded files.
    model.ensureLocalDebrisTmpLock("x");

    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Remotely remove d\7.
    ASSERT_TRUE(cd.deleteremote("x/d\7"));
    ASSERT_TRUE(model.movetosynctrash("x/d\7", "x"));

    // Locally remove f\7.
    auto syncRoot = TESTROOT / "cd" / "sd";
#ifdef _WIN32
    ASSERT_TRUE(fs::remove(syncRoot / "f%07"));
#else /* _WIN32 */
    ASSERT_TRUE(fs::remove(syncRoot / "f\7"));
#endif /* ! _WIN32 */
    ASSERT_TRUE(!!model.removenode("x/f\7"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Confirm models.
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Locally create some files with escapes in their names.
#ifdef _WIN32
    ASSERT_TRUE(fs::create_directories(syncRoot / "dd%07"));
    ASSERT_TRUE(createDataFile(syncRoot / "ff%07", "ff"));
#else
    ASSERT_TRUE(fs::create_directories(syncRoot / "dd\7"));
    ASSERT_TRUE(createDataFile(syncRoot / "ff\7", "ff"));
#endif /* ! _WIN32 */

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Update and confirm models.
    model.addfolder("x/dd\7");
    model.addfile("x/ff\7", "ff");

    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_RemotesWithEscapesSynchronizeCorrectly)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    // Populate cloud.
    {
        // Upload client.
        StandardClient cu(TESTROOT, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log in client and clear remote contents.
        ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));

        // Build test hierarchy.
        const auto root = TESTROOT / "cu" / "x";

        // Escapes will not be decoded as we're uploading directly.
        fs::create_directories(root / "d0");
        fs::create_directories(root / "d%30");

        ASSERT_TRUE(createNameFile(root, "f0"));
        ASSERT_TRUE(createNameFile(root, "f%30"));

        auto* node = cu.drillchildnodebyname(cu.gettestbasenode(), "x");
        ASSERT_TRUE(!!node);

        // Upload directories.
        ASSERT_TRUE(cu.uploadFolderTree(root / "d0", node));
        ASSERT_TRUE(cu.uploadFolderTree(root / "d%30", node));

        // Upload files.
        ASSERT_TRUE(cu.uploadFile(root / "f0", node));
        ASSERT_TRUE(cu.uploadFile(root / "f%30", node));
    }

    // Download client.
    StandardClient cd(TESTROOT, "cd");

    // Log callbacks.
    cd.logcb = true;

    // Log in client.
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Add and start sync.
    handle backupId1 = cd.setupSync_mainthread("sd", "x");

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Populate and confirm local fs.
    Model model;

    model.addfolder("x/d0");
    model.addfolder("x/d%30")->fsName("d%2530");
    model.addfile("x/f0", "f0");
    model.addfile("x/f%30", "f%30")->fsName("f%2530");

    // Needed as we've downloaded files.
    model.ensureLocalDebrisTmpLock("x");

    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Locally remove an escaped node.
    const auto syncRoot = cd.syncSet(backupId1).localpath;

    fs::remove_all(syncRoot / "d%2530");
    ASSERT_TRUE(!!model.removenode("x/d%30"));

    // Remotely remove an escaped file.
    ASSERT_TRUE(cd.deleteremote("x/f%30"));
    ASSERT_TRUE(model.movetosynctrash("x/f%30", "x"));

    // Wait for sync up to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Confirm models.
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Locally create some files with escapes in their names.
    {
        // Bogus escapes.
        ASSERT_TRUE(fs::create_directories(syncRoot / "dd%"));
        model.addfolder("x/dd%");

        ASSERT_TRUE(createNameFile(syncRoot, "ff%"));
        model.addfile("x/ff%", "ff%");

        // Sane character escapes.
        ASSERT_TRUE(fs::create_directories(syncRoot / "dd%31"));
        model.addfolder("x/dd1")->fsName("dd%31");

        ASSERT_TRUE(createNameFile(syncRoot, "ff%31"));
        model.addfile("x/ff1", "ff%31")->fsName("ff%31");

    }

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Confirm model.
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Let's try with escaped control sequences.
    ASSERT_TRUE(fs::create_directories(syncRoot / "dd%250a"));
    model.addfolder("x/dd%0a")->fsName("dd%250a");

    ASSERT_TRUE(createNameFile(syncRoot, "ff%250a"));
    model.addfile("x/ff%0a", "ff%250a")->fsName("ff%250a");

    // Wait for sync and confirm model.
    waitonsyncs(TIMEOUT, &cd);
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Remotely delete the nodes with control sequences.
    ASSERT_TRUE(cd.deleteremote("x/dd%0a"));
    model.movetosynctrash("x/dd%0a", "x");

    ASSERT_TRUE(cd.deleteremote("x/ff%0a"));
    model.movetosynctrash("x/ff%0a", "x");

    // Wait for sync and confirm model.
    waitonsyncs(TIMEOUT, &cd);
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));
}

#ifdef _WIN32
#define SEP "\\"
#else // _WIN32
#define SEP "/"
#endif // ! _WIN32

class AnomalyReporter
  : public FilenameAnomalyReporter
{
public:
    struct Anomaly
    {
        string localPath;
        string remotePath;
        int type;
    }; // Anomaly

    AnomalyReporter(const string& localRoot, const string& remoteRoot)
      : mAnomalies()
      , mLocalRoot(localRoot)
      , mRemoteRoot(remoteRoot)
    {
        assert(!mLocalRoot.empty());
        assert(!mRemoteRoot.empty());

        // Add trailing separators if necessary.
        if (string(1, mLocalRoot.back()) != SEP)
        {
            mLocalRoot.append(SEP);
        }

        if (mRemoteRoot.back() != '/')
        {
            mRemoteRoot.push_back('/');
        }
    }

    void anomalyDetected(FilenameAnomalyType type,
                         const string& localPath,
                         const string& remotePath) override
    {
        assert(startsWith(localPath, mLocalRoot));
        assert(startsWith(remotePath, mRemoteRoot));

        mAnomalies.emplace_back();

        auto& anomaly = mAnomalies.back();
        anomaly.localPath = localPath.substr(mLocalRoot.size());
        anomaly.remotePath = remotePath.substr(mRemoteRoot.size());
        anomaly.type = type;
    }

    vector<Anomaly> mAnomalies;

private:
    bool startsWith(const string& lhs, const string& rhs) const
    {
        return lhs.compare(0, rhs.size(), rhs) == 0;
    }

    string mLocalRoot;
    string mRemoteRoot;
}; // AnomalyReporter

TEST_F(SyncTest, AnomalousManualDownload)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = chrono::seconds(4);

    // Upload two files for us to download.
    {
        StandardClient cu(TESTROOT, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log client in.
        ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

        // Create a sync so we can upload some files.
        auto id = cu.setupSync_mainthread("s", "s");
        ASSERT_NE(id, UNDEF);

        // Get our hands on the sync root.
        auto root = cu.syncSet(id).localpath;

        // Create the test files.
        Model model;

        model.addfile("f");
        model.addfile("g:0")->fsName("g%3a0");
        model.generate(root);

        // Wait for the upload to complete.
        waitonsyncs(TIMEOUT, &cu);

        // Make sure the files were uploaded.
        ASSERT_TRUE(cu.confirmModel_mainthread(model.root.get(), id));
    }

    StandardClient cd(TESTROOT, "cd");

    // Log callbacks.
    cd.logcb = true;

    // Log client in.
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Determine root paths.
    auto root = TESTROOT / "cd";

    // Set anomalous filename reporter.
    AnomalyReporter* reporter =
      new AnomalyReporter(root.u8string(),
                          cd.gettestbasenode()->displaypath());

    cd.client.mFilenameAnomalyReporter.reset(reporter);

    // cu's sync root.
    auto* s = cd.drillchildnodebyname(cd.gettestbasenode(), "s");
    ASSERT_TRUE(s);

    // Simple validation helper.
    auto read_string = [](const fs::path& path) {
        // How much buffer space do we need?
        auto length = fs::file_size(path);
        assert(length > 0);

        // Read in the file's contents.
        ifstream istream(path.u8string(), ios::binary);
        string buffer(length, 0);

        istream.read(&buffer[0], length);

        // Make sure the read was successful.
        assert(istream.good());

        return buffer;
    };

    // Download a regular file.
    {
        // Regular file, s/f.
        auto* f = cd.drillchildnodebyname(s, "f");
        ASSERT_TRUE(f);

        // Download.
        auto destination = root / "f";
        ASSERT_TRUE(cd.downloadFile(*f, destination));

        // Make sure the file was downloaded.
        ASSERT_TRUE(fs::is_regular_file(destination));
        ASSERT_EQ(read_string(destination), "f");

        // No anomalies should be reported.
        ASSERT_TRUE(reporter->mAnomalies.empty());
    }

    // Download an anomalous file.
    {
        // Anomalous file, s/g:0.
        auto* g0 = cd.drillchildnodebyname(s, "g:0");
        ASSERT_TRUE(g0);

        // Download.
        auto destination = root / "g%3a0";
        ASSERT_TRUE(cd.downloadFile(*g0, destination));

        // Make sure the file was downloaded.
        ASSERT_TRUE(fs::is_regular_file(destination));
        ASSERT_EQ(read_string(destination), "g:0");

        // A single anomaly should be reported.
        ASSERT_EQ(reporter->mAnomalies.size(), 1);

        auto& anomaly = reporter->mAnomalies.front();

        ASSERT_EQ(anomaly.localPath, "g%3a0");
        ASSERT_EQ(anomaly.remotePath, "s/g:0");
        ASSERT_EQ(anomaly.type, FILENAME_ANOMALY_NAME_MISMATCH);
    }
}

TEST_F(SyncTest, AnomalousManualUpload)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = chrono::seconds(4);

    // Upload client.
    StandardClient cu(TESTROOT, "cu");

    // Verification client.
    StandardClient cv(TESTROOT, "cv");

    // Log callbacks.
    cu.logcb = true;
    cv.logcb = true;

    // Log in clients.
    ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));
    ASSERT_TRUE(cv.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Determine local root.
    auto root = TESTROOT / "cu";

    // Set up anomalous name reporter.
    AnomalyReporter* reporter =
      new AnomalyReporter(root.u8string(),
                          cu.gettestbasenode()->displaypath());

    cu.client.mFilenameAnomalyReporter.reset(reporter);

    // Create a sync so we can verify uploads.
    auto id = cv.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    Model model;

    // Upload a regular file.
    {
        // Add file to model.
        model.addfile("f0");
        model.generate(root);

        // Upload file.
        auto* s = cu.client.nodeByHandle(cv.syncSet(id).h);
        ASSERT_TRUE(s);
        ASSERT_TRUE(cu.uploadFile(root / "f0", s));

        // Necessary as cv has downloaded a file.
        model.ensureLocalDebrisTmpLock("");

        // Make sure the file uploaded successfully.
        waitonsyncs(TIMEOUT, &cv);

        ASSERT_TRUE(cv.confirmModel_mainthread(model.root.get(), id));

        // No anomalies should be reported.
        ASSERT_TRUE(reporter->mAnomalies.empty());
    }

    // Upload an anomalous file.
    {
        // Add an anomalous file.
        model.addfile("f:0")->fsName("f%3a0");
        model.generate(root);

        // Upload file.
        auto* s = cu.client.nodeByHandle(cv.syncSet(id).h);
        ASSERT_TRUE(s);
        ASSERT_TRUE(cu.uploadFile(root / "f%3a0", "f:0", s));

        // Make sure the file uploaded ok.
        waitonsyncs(TIMEOUT, &cv);

        ASSERT_TRUE(cv.confirmModel_mainthread(model.root.get(), id));

        // A single anomaly should've been reported.
        ASSERT_EQ(reporter->mAnomalies.size(), 1);

        auto& anomaly = reporter->mAnomalies.front();

        ASSERT_EQ(anomaly.localPath, "f%3a0");
        ASSERT_EQ(anomaly.remotePath, "s/f:0");
        ASSERT_EQ(anomaly.type, FILENAME_ANOMALY_NAME_MISMATCH);
    }
}

TEST_F(SyncTest, AnomalousSyncDownload)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = chrono::seconds(4);

    // For verification.
    Model model;

    // Upload test files.
    {
        // Upload client.
        StandardClient cu(TESTROOT, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log in client.
        ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

        // Add and start sync.
        auto id = cu.setupSync_mainthread("s", "s");
        ASSERT_NE(id, UNDEF);

        // Add test files for upload.
        auto root = cu.syncSet(id).localpath;

        model.addfile("f");
        model.addfile("f:0")->fsName("f%3a0");
        model.addfolder("d");
        model.addfolder("d:0")->fsName("d%3a0");
        model.generate(root);

        // Wait for sync to complete.
        waitonsyncs(TIMEOUT, &cu);

        // Did the files upload okay?
        ASSERT_TRUE(cu.confirmModel_mainthread(model.root.get(), id));
    }

    // Download test files.
    StandardClient cd(TESTROOT, "cd");

    // Log client in.
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Set anomalous filename reporter.
    AnomalyReporter* reporter;
    {
        auto* root = cd.gettestbasenode();
        ASSERT_TRUE(root);

        auto* s = cd.drillchildnodebyname(root, "s");
        ASSERT_TRUE(s);

        auto local = (TESTROOT / "cd" / "s").u8string();
        auto remote = s->displaypath();

        reporter = new AnomalyReporter(local, remote);
        cd.client.mFilenameAnomalyReporter.reset(reporter);
    }

    // Add and start sync.
    auto id = cd.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Necessary as cd has downloaded files.
    model.ensureLocalDebrisTmpLock("");

    // Were all the files downloaded okay?
    ASSERT_TRUE(cd.confirmModel_mainthread(model.root.get(), id));

    // Two anomalies should be reported.
    ASSERT_EQ(reporter->mAnomalies.size(), 2);

    auto anomaly = reporter->mAnomalies.begin();

    // d:0
    ASSERT_EQ(anomaly->localPath, "d%3a0");
    ASSERT_EQ(anomaly->remotePath, "d:0");
    ASSERT_EQ(anomaly->type, FILENAME_ANOMALY_NAME_MISMATCH);

    ++anomaly;

    // f:0
    ASSERT_EQ(anomaly->localPath, "f%3a0");
    ASSERT_EQ(anomaly->remotePath, "f:0");
    ASSERT_EQ(anomaly->type, FILENAME_ANOMALY_NAME_MISMATCH);
}

TEST_F(SyncTest, AnomalousSyncLocalRename)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT = chrono::seconds(4);

    // Sync client.
    StandardClient cx(TESTROOT, "cx");

    // Log in client.
    ASSERT_TRUE(cx.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    auto id = cx.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    auto root = cx.syncSet(id).localpath;

    // Set anomalous filename reporter.
    AnomalyReporter* reporter =
      new AnomalyReporter(root.u8string(), "/mega_test_sync/s");

    cx.client.mFilenameAnomalyReporter.reset(reporter);

    // Populate filesystem.
    Model model;

    model.addfile("d/f");
    model.addfile("f");
    model.generate(root);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cx);

    // Make sure everything uploaded okay.
    ASSERT_TRUE(cx.confirmModel_mainthread(model.root.get(), id));

    // Rename d/f -> d/g.
    model.findnode("d/f")->name = "g";
    fs::rename(root / "d" / "f", root / "d" / "g");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cx);

    // Confirm move.
    ASSERT_TRUE(cx.confirmModel_mainthread(model.root.get(), id));

    // There should be no anomalies.
    ASSERT_TRUE(reporter->mAnomalies.empty());

    // Rename d/g -> d/g:0.
    model.findnode("d/g")->fsName("g%3a0").name = "g:0";
    fs::rename(root / "d" / "g", root / "d" / "g%3a0");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cx);

    // Confirm move.
    ASSERT_TRUE(cx.confirmModel_mainthread(model.root.get(), id));

    // There should be a single anomaly.
    ASSERT_EQ(reporter->mAnomalies.size(), 1);
    {
        auto& anomaly = reporter->mAnomalies.back();

        ASSERT_EQ(anomaly.localPath, "d" SEP "g%3a0");
        ASSERT_EQ(anomaly.remotePath, "d/g:0");
        ASSERT_EQ(anomaly.type, FILENAME_ANOMALY_NAME_MISMATCH);
    }
    reporter->mAnomalies.clear();

    // Move f -> d/g:0.
    model.findnode("d/g:0")->content = "f";
    model.removenode("f");
    fs::rename(root / "f", root / "d" / "g%3a0");

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &cx);

    // Confirm move.
    ASSERT_TRUE(cx.confirmModel_mainthread(model.root.get(), id));

    // No anomalies should be reported.
    ASSERT_TRUE(reporter->mAnomalies.empty());
}

TEST_F(SyncTest, AnomalousSyncRemoteRename)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT = chrono::seconds(4);

    // Sync client.
    StandardClient cx(TESTROOT, "cx");

    // Rename client.
    StandardClient cr(TESTROOT, "cr");

    // Log in clients.
    ASSERT_TRUE(cx.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));
    ASSERT_TRUE(cr.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Add and start sync.
    auto id = cx.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    auto root = cx.syncSet(id).localpath;

    // Set up anomalous filename reporter.
    auto* reporter = new AnomalyReporter(root.u8string(), "/mega_test_sync/s");
    cx.client.mFilenameAnomalyReporter.reset(reporter);

    // Populate filesystem.
    Model model;

    model.addfile("d/f");
    model.addfile("f");
    model.generate(root);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &cx);

    // Verify upload.
    ASSERT_TRUE(cx.confirmModel_mainthread(model.root.get(), id));

    // Rename d/f -> d/g.
    auto* s = cr.client.nodeByHandle(cx.syncSet(id).h);
    ASSERT_TRUE(s);

    auto* d = cr.drillchildnodebyname(s, "d");
    ASSERT_TRUE(d);

    {
        auto* f = cr.drillchildnodebyname(d, "f");
        ASSERT_TRUE(f);

        ASSERT_TRUE(cr.setattr(f, attr_map('n', "g")));
    }

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &cx);

    // Update model.
    model.findnode("d/f")->name = "g";

    // Verify rename.
    ASSERT_TRUE(cx.confirmModel_mainthread(model.root.get(), id));

    // There should be no anomalies.
    ASSERT_TRUE(reporter->mAnomalies.empty());

    // Rename d/g -> d/g:0.
    {
        auto* g = cr.drillchildnodebyname(d, "g");
        ASSERT_TRUE(g);

        ASSERT_TRUE(cr.setattr(g, attr_map('n', "g:0")));
    }

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &cx);

    // Update model.
    model.findnode("d/g")->fsName("g%3a0").name = "g:0";

    // Verify rename.
    ASSERT_TRUE(cx.confirmModel_mainthread(model.root.get(), id));

    // There should be a single anomaly.
    ASSERT_EQ(reporter->mAnomalies.size(), 1);
    {
        auto& anomaly = reporter->mAnomalies.back();

        ASSERT_EQ(anomaly.localPath, "d" SEP "g%3a0");
        ASSERT_EQ(anomaly.remotePath, "d/g:0");
        ASSERT_EQ(anomaly.type, FILENAME_ANOMALY_NAME_MISMATCH);
    }
    reporter->mAnomalies.clear();
}

TEST_F(SyncTest, AnomalousSyncUpload)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT = chrono::seconds(4);

    // Upload client.
    StandardClient cu(TESTROOT, "cu");

    // Log client in.
    ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    auto id = cu.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    auto root = cu.syncSet(id).localpath;

    // Set up anomalous filename reporter.
    AnomalyReporter* reporter =
      new AnomalyReporter(root.u8string(), "/mega_test_sync/s");

    cu.client.mFilenameAnomalyReporter.reset(reporter);

    // Populate filesystem.
    Model model;

    model.addfile("f");
    model.addfile("f:0")->fsName("f%3a0");
    model.addfolder("d");
    model.addfolder("d:0")->fsName("d%3a0");
    model.generate(root);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cu);

    // Ensure everything uploaded okay.
    ASSERT_TRUE(cu.confirmModel_mainthread(model.root.get(), id));

    // Two anomalies should've been reported.
    ASSERT_EQ(reporter->mAnomalies.size(), 2);

    auto anomaly = reporter->mAnomalies.begin();

    // d:0
    ASSERT_EQ(anomaly->localPath, "d%3a0");
    ASSERT_EQ(anomaly->remotePath, "d:0");
    ASSERT_EQ(anomaly->type, FILENAME_ANOMALY_NAME_MISMATCH);

    ++anomaly;

    // f:0
    ASSERT_EQ(anomaly->localPath, "f%3a0");
    ASSERT_EQ(anomaly->remotePath, "f:0");
    ASSERT_EQ(anomaly->type, FILENAME_ANOMALY_NAME_MISMATCH);
}

#undef SEP

TEST_F(SyncTest, BasicSyncExportImport)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = chrono::seconds(4);

    // Sync client.
    unique_ptr<StandardClient> cx(new StandardClient(TESTROOT, "cx"));

    // Log callbacks.
    cx->logcb = true;

    // Log in client.
    ASSERT_TRUE(cx->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 1, 3));

    // Create and start syncs.
    auto id0 = cx->setupSync_mainthread("s0", "s/s_0");
    ASSERT_NE(id0, UNDEF);

    auto id1 = cx->setupSync_mainthread("s1", "s/s_1");
    ASSERT_NE(id1, UNDEF);

    auto id2 = cx->setupSync_mainthread("s2", "s/s_2");
    ASSERT_NE(id2, UNDEF);

    // Get our hands on the sync's local root.
    auto root0 = cx->syncSet(id0).localpath;
    auto root1 = cx->syncSet(id1).localpath;
    auto root2 = cx->syncSet(id2).localpath;

    // Give the syncs something to synchronize.
    Model model0;
    Model model1;
    Model model2;

    model0.addfile("d0/f0");
    model0.addfile("f0");
    model0.generate(root0);

    model1.addfile("d0/f0");
    model1.addfile("d0/f1");
    model1.addfile("d1/f0");
    model1.addfile("d1/f1");
    model1.generate(root1);

    model2.addfile("f0");
    model2.addfile("f1");
    model2.generate(root2);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, cx.get());

    // Make sure everything was uploaded okay.
    ASSERT_TRUE(cx->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(cx->confirmModel_mainthread(model1.root.get(), id1));
    ASSERT_TRUE(cx->confirmModel_mainthread(model2.root.get(), id2));

    // Export the syncs.
    auto configs = cx->exportSyncConfigs();
    ASSERT_FALSE(configs.empty());

    // Log out client, don't keep caches.
    cx.reset();

    // Recreate client.
    cx.reset(new StandardClient(TESTROOT, "cx"));

    // Log client back in.
    ASSERT_TRUE(cx->login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Import the syncs.
    ASSERT_TRUE(cx->importSyncConfigs(std::move(configs)));

    // Determine the imported sync's backup IDs.
    id0 = cx->backupIdForSyncPath(root0);
    ASSERT_NE(id0, UNDEF);

    id1 = cx->backupIdForSyncPath(root1);
    ASSERT_NE(id1, UNDEF);

    id2 = cx->backupIdForSyncPath(root2);
    ASSERT_NE(id2, UNDEF);

    // Make sure nothing's changed since we exported the syncs.
    ASSERT_TRUE(cx->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(cx->confirmModel_mainthread(model1.root.get(), id1));
    ASSERT_TRUE(cx->confirmModel_mainthread(model2.root.get(), id2));

    // Make some changes.
    model0.addfile("d0/f1");
    model0.generate(root0);

    model1.addfile("f0");
    model1.generate(root1);

    model2.addfile("d0/d0f0");
    model2.generate(root2);

    // Imported syncs should be disabled.
    // So, we're waiting for the syncs to do precisely nothing.
    waitonsyncs(TIMEOUT, cx.get());

    // Confirm should fail.
    ASSERT_FALSE(cx->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_FALSE(cx->confirmModel_mainthread(model1.root.get(), id1));
    ASSERT_FALSE(cx->confirmModel_mainthread(model2.root.get(), id2));

    // Enable the imported syncs.
    ASSERT_TRUE(cx->enableSyncByBackupId(id0));
    ASSERT_TRUE(cx->enableSyncByBackupId(id1));
    ASSERT_TRUE(cx->enableSyncByBackupId(id2));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, cx.get());

    // Changes should now be in the cloud.
    ASSERT_TRUE(cx->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(cx->confirmModel_mainthread(model1.root.get(), id1));
    ASSERT_TRUE(cx->confirmModel_mainthread(model2.root.get(), id2));
}

TEST_F(SyncTest, RenameReplaceFileBetweenSyncs)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c0(TESTROOT, "c0");

    // Log callbacks.
    c0.logcb = true;

    // Log in client.
    ASSERT_TRUE(c0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s0", 0, 0));
    ASSERT_TRUE(c0.makeCloudSubdirs("s1", 0, 0));

    // Set up syncs.
    const auto id0 = c0.setupSync_mainthread("s0", "s0");
    ASSERT_NE(id0, UNDEF);

    const auto id1 = c0.setupSync_mainthread("s1", "s1");
    ASSERT_NE(id1, UNDEF);

    // Convenience.
    const auto SYNCROOT0 = TESTROOT / "c0" / "s0";
    const auto SYNCROOT1 = TESTROOT / "c0" / "s1";

    // Set up models.
    Model model0;
    Model model1;

    model0.addfile("f0", "x");
    model0.generate(SYNCROOT0);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0.confirmModel_mainthread(model1.root.get(), id1));

    // Move s0/f0 to s1/f0.
    model1 = model0;

    fs::rename(SYNCROOT0 / "f0", SYNCROOT1 / "f0");

    // Replace s0/f0.
    model0.removenode("f0");
    model0.addfile("f0", "y");

    ASSERT_TRUE(createDataFile(SYNCROOT0 / "f0", "y"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0.confirmModel_mainthread(model1.root.get(), id1));

    // Disable s0.
    ASSERT_TRUE(c0.disableSync(id0, NO_SYNC_ERROR, false));

    // Make sure s0 is disabled.
    ASSERT_TRUE(createDataFile(SYNCROOT0 / "f1", "z"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    // Move s1/f0 to s0/f2.
    model1.removenode("f0");

    fs::rename(SYNCROOT1 / "f0", SYNCROOT0 / "f2");

    // Replace s1/f0.
    model1.addfile("f0", "q");

    ASSERT_TRUE(createDataFile(SYNCROOT1 / "f0", "q"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    ASSERT_TRUE(c0.confirmModel_mainthread(model1.root.get(), id1));
}

TEST_F(SyncTest, RenameReplaceFileWithinSync)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c0(TESTROOT, "c0");

    // Log callbacks.
    c0.logcb = true;

    // Log in client and clear remote contents.
    ASSERT_TRUE(c0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s0", 0, 0));

    // Set up sync.
    const auto id = c0.setupSync_mainthread("s0", "s0");
    ASSERT_NE(id, UNDEF);

    // Populate local FS.
    const auto SYNCROOT = TESTROOT / "c0" / "s0";

    Model model;

    model.addfile("f1");
    model.generate(SYNCROOT);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm model.
    ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id));

    // Rename /f1 to /f2.
    // This tests the case where the target is processed after the source.
    model.addfile("f2", "f1");
    model.removenode("f1");

    fs::rename(SYNCROOT / "f1", SYNCROOT / "f2");

    // Replace /d1.
    model.addfile("f1", "x");

    ASSERT_TRUE(createDataFile(SYNCROOT / "f1", "x"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm model.
    ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id));

    // Rename /f2 to /f0.
    // This tests the case where the target is processed before the source.
    model.addfile("f0", "f1");
    model.removenode("f2");

    fs::rename(SYNCROOT / "f2", SYNCROOT / "f0");

    // Replace /d2.
    model.addfile("f2", "y");

    ASSERT_TRUE(createDataFile(SYNCROOT / "f2", "y"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm model.
    ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id));
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_RenameReplaceFolderBetweenSyncs)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c0(TESTROOT, "c0");

    // Log callbacks.
    c0.logcb = true;

    // Log in client.
    ASSERT_TRUE(c0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s0", 0, 0));
    ASSERT_TRUE(c0.makeCloudSubdirs("s1", 0, 0));

    // Set up syncs.
    const auto id0 = c0.setupSync_mainthread("s0", "s0");
    ASSERT_NE(id0, UNDEF);

    const auto id1 = c0.setupSync_mainthread("s1", "s1");
    ASSERT_NE(id1, UNDEF);

    // Convenience.
    const auto SYNCROOT0 = TESTROOT / "c0" / "s0";
    const auto SYNCROOT1 = TESTROOT / "c0" / "s1";

    // Set up models.
    Model model0;
    Model model1;

    model0.addfile("d0/f0");
    model0.generate(SYNCROOT0);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0.confirmModel_mainthread(model1.root.get(), id1));

    // Move s0/d0 to s1/d0. (and replace)
    model1 = model0;

    fs::rename(SYNCROOT0 / "d0", SYNCROOT1 / "d0");

    // Replace s0/d0.
    model0.removenode("d0/f0");

    fs::create_directories(SYNCROOT0 / "d0");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0.confirmModel_mainthread(model1.root.get(), id1));

    // Disable s0.
    ASSERT_TRUE(c0.disableSync(id0, NO_SYNC_ERROR, false));

    // Make sure s0 is disabled.
    fs::create_directories(SYNCROOT0 / "d1");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    // Move s1/d0 to s0/d2.
    model1.removenode("d0/f0");

    fs::rename(SYNCROOT1 / "d0", SYNCROOT0 / "d2");

    // Replace s1/d0.
    fs::create_directories(SYNCROOT1 / "d0");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm models.
    ASSERT_TRUE(c0.confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    ASSERT_TRUE(c0.confirmModel_mainthread(model1.root.get(), id1));
}

TEST_F(SyncTest, RenameReplaceFolderWithinSync)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c0(TESTROOT, "c0");

    // Log callbacks.
    c0.logcb = true;

    // Log in client and clear remote contents.
    ASSERT_TRUE(c0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s0", 0, 0));

    // Set up sync.
    const auto id = c0.setupSync_mainthread("s0", "s0");
    ASSERT_NE(id, UNDEF);

    // Populate local FS.
    const auto SYNCROOT = TESTROOT / "c0" / "s0";

    Model model;

    model.addfile("d1/f0");
    model.generate(SYNCROOT);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm model.
    ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id));

    // Rename /d1 to /d2.
    // This tests the case where the target is processed after the source.
    model.addfolder("d2");
    model.movenode("d1/f0", "d2");

    fs::rename(SYNCROOT / "d1", SYNCROOT / "d2");

    // Replace /d1.
    fs::create_directories(SYNCROOT / "d1");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm model.
    ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id));

    // Rename /d2 to /d0.
    // This tests the case where the target is processed before the source.
    model.addfolder("d0");
    model.movenode("d2/f0", "d0");

    fs::rename(SYNCROOT / "d2", SYNCROOT / "d0");

    // Replace /d2.
    fs::create_directories(SYNCROOT / "d2");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c0);

    // Confirm model.
    ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, DownloadedDirectoriesHaveFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Create /d in the cloud.
    {
        vector<NewNode> nodes(1);

        // Initialize new node.
        c.client.putnodes_prepareOneFolder(&nodes[0], "d");

        // Get our hands on the sync root.
        auto* root = c.drillchildnodebyname(c.gettestbasenode(), "s");
        ASSERT_TRUE(root);

        // Create new node in the cloud.
        ASSERT_TRUE(c.putnodes(root->nodeHandle(), std::move(nodes)));
    }

    // Add and start sync.
    const auto id = c.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c.syncSet(id).localpath;

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c);

    // Confirm /d has made it to disk.
    Model model;

    model.addfolder("d");

    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Trigger a filesystem notification.
    model.addfile("d/f", "x");

    ASSERT_TRUE(createDataFile(SYNCROOT / "d" / "f", "x"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c);

    // Confirm /d/f made it to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, FilesystemWatchesPresentAfterResume)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    auto c = ::mega::make_unique<StandardClient>(TESTROOT, "c");

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    const auto id = c->setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c->syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("d0/d0d0");
    model.generate(SYNCROOT);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, c.get());

    // Make sure directories made it to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Logout / Resume.
    {
        string session;

        // Save session.
        c->client.dumpsession(session);

        // Logout (taking care to preserve the caches.)
        c->localLogout();

        // Resume session.
        c.reset(new StandardClient(TESTROOT, "c"));
        ASSERT_TRUE(c->login_fetchnodes(session));

        // Wait for sync to complete.
        waitonsyncs(TIMEOUT, c.get());

        // Make sure everything's as we left it.
        ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
    }

    // Trigger some filesystem notifications.
    {
        model.addfile("f", "f");
        ASSERT_TRUE(createDataFile(SYNCROOT / "f", "f"));

        model.addfile("d0/d0f", "d0f");
        ASSERT_TRUE(createDataFile(SYNCROOT / "d0" / "d0f", "d0f"));

        model.addfile("d0/d0d0/d0d0f", "d0d0f");
        ASSERT_TRUE(createDataFile(SYNCROOT / "d0" / "d0d0" / "d0d0f", "d0d0f"));
    }

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c.get());

    // Did the new files make it to the cloud?
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, MoveTargetHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Set up sync.
    const auto id = c.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c.syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("d0/dq");
    model.addfolder("d1");
    model.addfolder("d2/dx");
    model.generate(SYNCROOT);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Confirm directories have hit the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Local move.
    {
        // d0/dq -> d1/dq (ascending.)
        model.movenode("d0/dq", "d1");

        fs::rename(SYNCROOT / "d0" / "dq",
                   SYNCROOT / "d1" / "dq");

        // d2/dx -> d1/dx (descending.)
        model.movenode("d2/dx", "d1");

        fs::rename(SYNCROOT / "d2" / "dx",
                   SYNCROOT / "d1" / "dx");
    }

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure movement has propagated to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Trigger some filesystem notifications.
    model.addfile("d1/dq/fq", "q");
    model.addfile("d1/dx/fx", "x");

    ASSERT_TRUE(createDataFile(SYNCROOT / "d1" / "dq" / "fq", "q"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "d1" / "dx" / "fx", "x"));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Have the files made it up to the cloud?
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Remotely move.
    {
        StandardClient cr(TESTROOT, "cr");

        // Log in client.
        ASSERT_TRUE(cr.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // d1/dq -> d2/dq (ascending.)
        model.movenode("d1/dq", "d2");

        ASSERT_TRUE(cr.movenode("s/d1/dq", "s/d2"));

        // d1/dx -> d0/dx (descending.)
        model.movenode("d1/dx", "d0");

        ASSERT_TRUE(cr.movenode("s/d1/dx", "s/d0"));
    }

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure movements occured on disk.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Trigger some filesystem notifications.
    model.removenode("d2/dq/fq");
    model.removenode("d0/dx/fx");

    fs::remove(SYNCROOT / "d2" / "dq" / "fq");
    fs::remove(SYNCROOT / "d0" / "dx" / "fx");

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure removes propagated to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_DeleteReplaceReplacementHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    const auto id = c.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    const auto ROOT = c.syncSet(id).localpath;

    // Populate filesystem.
    Model model;

    model.addfolder("dx/f");
    model.generate(ROOT);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure the directory's been uploaded to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Remove/replace the directory.
    fs::remove_all(ROOT / "dx");
    fs::create_directory(ROOT / "dx");

    // Wait for all notifications to be processed.
    waitonsyncs(TIMEOUT, &c);

    // Make sure the new directory is in the cloud.
    model.removenode("dx/f");

    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Add a file in the new directory so we trigger a notification.
    model.addfile("dx/g", "g");

    ASSERT_TRUE(createDataFile(ROOT / "dx" / "g", "g"));

    // Wait for notifications to be processed.
    waitonsyncs(TIMEOUT, &c);

    // Check if g has been uploaded.
    // If it hasn't, we probably didn't receive a notification from the filesystem.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, RenameReplaceSourceAndTargetHaveFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(8);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    const auto id = c.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c.syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("dq");
    model.addfolder("dz");
    model.generate(SYNCROOT);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure directories have made it to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Rename /dq -> /dr (ascending), replace /dq.
    model.addfolder("dr");

    fs::rename(SYNCROOT / "dq", SYNCROOT / "dr");
    fs::create_directories(SYNCROOT / "dq");

    // Rename /dz -> /dy (descending), replace /dz.
    model.addfolder("dy");

    fs::rename(SYNCROOT / "dz", SYNCROOT / "dy");
    fs::create_directories(SYNCROOT / "dz");

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure moves made it to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Make sure rename targets still receive notifications.
    model.addfile("dr/fr", "r");
    model.addfile("dy/fy", "y");

    ASSERT_TRUE(createDataFile(SYNCROOT / "dr" / "fr", "r"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "dy" / "fy", "y"));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Did the files make it to the cloud?
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Make sure (now replaced) rename sources still receive notifications.
    model.addfile("dq/fq", "q");
    model.addfile("dz/fz", "z");

    LOG_debug << " --- Creating files fq and fz now ----";

    ASSERT_TRUE(createDataFile(SYNCROOT / "dq" / "fq", "q"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "dz" / "fz", "z"));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Did the files make it to the cloud?
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, RenameTargetHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    const auto id = c.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c.syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("dq");
    model.addfolder("dz");
    model.generate(SYNCROOT);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c);

    // Confirm model.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Locally rename.
    {
        // - dq -> dr (ascending)
        model.removenode("dq");
        model.addfolder("dr");

        fs::rename(SYNCROOT / "dq", SYNCROOT / "dr");

        // - dz -> dy (descending)
        model.removenode("dz");
        model.addfolder("dy");

        fs::rename(SYNCROOT / "dz", SYNCROOT / "dy");
    }

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure rename has hit the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Make sure rename targets receive notifications.
    model.addfile("dr/f", "x");
    model.addfile("dy/f", "y");

    ASSERT_TRUE(createDataFile(SYNCROOT / "dr" / "f", "x"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "dy" / "f", "y"));

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c);

    // Check file has made it to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Remotely rename.
    {
        StandardClient cr(TESTROOT, "cc");

        // Log in client.
        ASSERT_TRUE(cr.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        auto* root = cr.gettestbasenode();
        ASSERT_TRUE(root);

        // dr -> ds (ascending.)
        model.removenode("dr");
        model.addfile("ds/f", "x");

        auto* dr = cr.drillchildnodebyname(root, "s/dr");
        ASSERT_TRUE(dr);

        ASSERT_TRUE(cr.setattr(dr, attr_map('n', "ds")));

        // dy -> dx (descending.)
        model.removenode("dy");
        model.addfile("dx/f", "y");

        auto* dy = cr.drillchildnodebyname(root, "s/dy");
        ASSERT_TRUE(dy);

        ASSERT_TRUE(cr.setattr(dy, attr_map('n', "dx")));
    }

    WaitMillisec(4000); // it can take a while for APs to arrive (or to be sent)

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c);

    // Confirm move has occured locally.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Check that /ds and /dx receive notifications.
    model.removenode("ds/f");
    model.removenode("dx/f");

    fs::remove(SYNCROOT / "ds" / "f");
    fs::remove(SYNCROOT / "dx" / "f");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &c);

    // Confirm remove has hit the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, RootHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client and clear remote contents.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Set up sync
    const auto id = c.setupSync_mainthread("s", "s");
    ASSERT_NE(id, UNDEF);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Trigger some filesystem notifications.
    Model model;

    model.addfolder("d0");
    model.addfile("f0");
    model.generate(c.syncSet(id).localpath);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Confirm models.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

struct TwoWaySyncSymmetryCase
{
    enum SyncType { type_twoWay, type_backupSync, type_numTypes };

    enum Action { action_rename, action_moveWithinSync, action_moveOutOfSync, action_moveIntoSync, action_delete, action_numactions };

    enum MatchState { match_exact,      // the sync destination has the exact same file/folder at the same relative path
                      match_older,      // the sync destination has an older file/folder at the same relative path
                      match_newer,      // the sync destination has a newer file/folder at the same relative path
                      match_absent };   // the sync destination has no node at the same relative path

    SyncType syncType = type_twoWay;
    Action action = action_rename;
    bool selfChange = false; // changed by our own client or another
    bool up = false;  // or down - sync direction
    bool file = false;  // or folder.  Which one this test changes
    bool isExternal = false;
    bool pauseDuringAction = false;
    Model localModel;
    Model remoteModel;
    handle backupId = UNDEF;

    bool printTreesBeforeAndAfter = false;

    struct State
    {
        StandardClient& steadyClient;
        StandardClient& resumeClient;
        StandardClient& nonsyncClient;
        fs::path localBaseFolderSteady;
        fs::path localBaseFolderResume;
        std::string remoteBaseFolder = "twoway";   // leave out initial / so we can drill down from root node

        State(StandardClient& ssc, StandardClient& rsc, StandardClient& sc2) : steadyClient(ssc), resumeClient(rsc), nonsyncClient(sc2) {}
    };

    State& state;
    TwoWaySyncSymmetryCase(State& wholestate) : state(wholestate) {}

    std::string typeName()
    {
        switch (syncType)
        {
        case type_twoWay:
            return "twoWay_";
        case type_backupSync:
            return isExternal ? "external_backup_"
                              : "internal_backup_";
        default:
            assert(false);
            return "";
        }
    }

    std::string actionName()
    {
        switch (action)
        {
        case action_rename: return "rename";
        case action_moveWithinSync: return "move";
        case action_moveOutOfSync: return "moveOut";
        case action_moveIntoSync: return "moveIn";
        case action_delete: return "delete";
        default: assert(false); return "";
        }
    }

    std::string matchName(MatchState m)
    {
        switch (m)
        {
            case match_exact: return "exact";
            case match_older: return "older";
            case match_newer: return "newer";
            case match_absent: return "absent";
        }
        return "bad enum";
    }

    std::string name()
    {
        return  typeName() + actionName() +
                (up?"_up" : "_down") +
                (selfChange?"_self":"_other") +
                (file?"_file":"_folder") +
                (pauseDuringAction?"_resumed":"_steady");
    }

    fs::path localTestBasePathSteady;
    fs::path localTestBasePathResume;
    std::string remoteTestBasePath;

    Model& sourceModel() { return up ? localModel : remoteModel; }
    Model& destinationModel() { return up ? remoteModel : localModel; }

    StandardClient& client1() { return pauseDuringAction ? state.resumeClient : state.steadyClient; }
    StandardClient& changeClient() { return selfChange ? client1() : state.nonsyncClient; }

    fs::path localTestBasePath() { return pauseDuringAction ? localTestBasePathResume : localTestBasePathSteady; }

    bool CopyLocalTree(const fs::path& destination, const fs::path& source) try
    {
        using PathPair = std::pair<fs::path, fs::path>;

        // Assume we've already copied if the destination exists.
        if (fs::exists(destination)) return true;

        std::list<PathPair> pending;

        pending.emplace_back(destination, source);

        for (; !pending.empty(); pending.pop_front())
        {
            const auto& dst = pending.front().first;
            const auto& src = pending.front().second;

            // Assume target directory doesn't exist.
            fs::create_directories(dst);

            // Iterate over source directory's children.
            auto i = fs::directory_iterator(src);
            auto j = fs::directory_iterator();

            for ( ; i != j; ++i)
            {
                auto from = i->path();
                auto to = dst / from.filename();

                // If it's a file, copy it and preserve its modification time.
                if (fs::is_regular_file(from))
                {
                    // Copy the file.
                    fs::copy_file(from, to);

                    // Preserve modification time.
                    fs::last_write_time(to, fs::last_write_time(from));

                    // Process next child.
                    continue;
                }

                // If it's not a file, it must be a directory.
                assert(fs::is_directory(from));

                // So, create it!
                fs::create_directories(to);

                // And copy its content.
                pending.emplace_back(to, from);
            }
        }

        return true;
    }
    catch (...)
    {
        return false;
    }

    // prepares a local folder for testing, which will be two-way synced before the test
    void SetupForSync()
    {
        // Prepare Cloud
        {
            remoteTestBasePath = state.remoteBaseFolder + "/" + name();

            auto& client = changeClient();

            auto* root = client.gettestbasenode();
            ASSERT_NE(root, nullptr);

            root = client.drillchildnodebyname(root, state.remoteBaseFolder);
            ASSERT_NE(root, nullptr);

            auto* from = client.drillchildnodebyname(root, "initial");
            ASSERT_NE(from, nullptr);

            ASSERT_TRUE(client.cloudCopyTreeAs(from, root, name()));
        }

        // Prepare Local Filesystem
        {
            localTestBasePathSteady = state.localBaseFolderSteady / name();
            localTestBasePathResume = state.localBaseFolderResume / name();

            auto from = state.nonsyncClient.fsBasePath / "twoway" / "initial";
            ASSERT_TRUE(CopyLocalTree(localTestBasePathResume, from));
            ASSERT_TRUE(CopyLocalTree(localTestBasePathSteady, from));

            ASSERT_TRUE(CopyLocalTree(state.localBaseFolderResume / "initial", from));
            ASSERT_TRUE(CopyLocalTree(state.localBaseFolderSteady / "initial", from));
        }

        // Prepare models.
        {
            localModel.root->addkid(localModel.buildModelSubdirs("f", 2, 2, 2));
            localModel.root->addkid(localModel.buildModelSubdirs("outside", 2, 1, 1));
            localModel.addfile("f/file_older_1", "file_older_1");
            localModel.addfile("f/file_older_2", "file_older_2");
            localModel.addfile("f/file_newer_1", "file_newer_1");
            localModel.addfile("f/file_newer_2", "file_newer_2");
            remoteModel = localModel;
        }
    }

    bool isBackup() const
    {
        return syncType == type_backupSync;
    }

    bool isExternalBackup() const
    {
        return isExternal && isBackup();
    }

    bool isInternalBackup() const
    {
        return !isExternal && isBackup();
    }

    bool shouldRecreateOnResume() const
    {
        if (pauseDuringAction)
        {
            return isExternalBackup();
        }

        return false;
    }

    bool shouldDisableSync() const
    {
        if (up)
        {
            return false;
        }

        if (pauseDuringAction)
        {
            return isInternalBackup();
        }

        return isBackup();
    }

    bool shouldUpdateDestination() const
    {
        return up || !isBackup();
    }

    bool shouldUpdateModel() const
    {
        return up
               || !pauseDuringAction
               || !isExternalBackup();
    }

    fs::path localSyncRootPath()
    {
        return localTestBasePath() / "f";
    }

    string remoteSyncRootPath()
    {
        return remoteTestBasePath + "/f";
    }

    Node* remoteSyncRoot()
    {
        Node* root = client1().client.nodebyhandle(client1().basefolderhandle);
        if (root)
        {
            return client1().drillchildnodebyname(root, remoteSyncRootPath());
        }

        return nullptr;
    }

    handle BackupAdd(const string& drivePath, const string& sourcePath, const string& targetPath)
    {
        return client1().backupAdd_mainthread(drivePath, sourcePath, targetPath);
    }

    handle SetupSync(const string& sourcePath, const string& targetPath)
    {
        return client1().setupSync_mainthread(sourcePath, targetPath, isBackup());
    }

    void SetupTwoWaySync()
    {
        ASSERT_NE(remoteSyncRoot(), nullptr);

        string basePath   = client1().fsBasePath.u8string();
        string drivePath  = localTestBasePath().u8string();
        string sourcePath = localSyncRootPath().u8string();
        string targetPath = remoteSyncRootPath();

        drivePath.erase(0, basePath.size() + 1);
        sourcePath.erase(0, basePath.size() + 1);

        if (isExternalBackup())
        {
            backupId = BackupAdd(drivePath, sourcePath, targetPath);
        }
        else
        {
            backupId = SetupSync(sourcePath, targetPath);
        }

        ASSERT_NE(backupId, UNDEF);

        if (Sync* sync = client1().syncByBackupId(backupId))
        {
            sync->syncname += "/" + name() + " ";
        }
    }

    void PauseTwoWaySync()
    {
        if (shouldRecreateOnResume())
        {
            client1().delSync_mainthread(backupId, true);
        }
    }

    void ResumeTwoWaySync()
    {
        if (shouldRecreateOnResume())
        {
            SetupTwoWaySync();
        }
    }

    void remote_rename(std::string nodepath, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (deleteTargetFirst) remote_delete(parentpath(nodepath) + "/" + newname, updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) remoteModel.emulate_rename(nodepath, newname);

        Node* testRoot = changeClient().client.nodebyhandle(client1().basefolderhandle);
        Node* n = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        ASSERT_TRUE(!!n);

        if (reportaction) out() << name() << " action: remote rename " << n->displaypath() << " to " << newname;

        attr_map updates('n', newname);
        auto e = changeClient().client.setattr(n, move(updates), ++next_request_tag, nullptr, nullptr);

        ASSERT_EQ(API_OK, error(e));
    }

    void remote_move(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (deleteTargetFirst) remote_delete(newparentpath + "/" + leafname(nodepath), updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) remoteModel.emulate_move(nodepath, newparentpath);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote move " << n1->displaypath() << " to " << n2->displaypath();

        auto e = changeClient().client.rename(n1, n2, SYNCDEL_NONE, NodeHandle(), nullptr, nullptr);
        ASSERT_EQ(API_OK, e);
    }

    void remote_copy(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (updatemodel) remoteModel.emulate_copy(nodepath, newparentpath);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote copy " << n1->displaypath() << " to " << n2->displaypath();

        TreeProcCopy tc;
        changeClient().client.proctree(n1, &tc, false, true);
        tc.allocnodes();
        changeClient().client.proctree(n1, &tc, false, true);
        tc.nn[0].parenthandle = UNDEF;

        SymmCipher key;
        AttrMap attrs;
        string attrstring;
        key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
        attrs = n1->attrs;
        attrs.getjson(&attrstring);
        client1().client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
        changeClient().client.putnodes(n2->nodeHandle(), move(tc.nn), nullptr, ++next_request_tag);
    }

    void remote_renamed_copy(std::string nodepath, std::string newparentpath, string newname, bool updatemodel, bool reportaction)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (updatemodel)
        {
            remoteModel.emulate_rename_copy(nodepath, newparentpath, newname);
        }

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote rename + copy " << n1->displaypath() << " to " << n2->displaypath() << " as " << newname;

        TreeProcCopy tc;
        changeClient().client.proctree(n1, &tc, false, true);
        tc.allocnodes();
        changeClient().client.proctree(n1, &tc, false, true);
        tc.nn[0].parenthandle = UNDEF;

        SymmCipher key;
        AttrMap attrs;
        string attrstring;
        key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
        attrs = n1->attrs;
        client1().client.fsaccess->normalize(&newname);
        attrs.map['n'] = newname;
        attrs.getjson(&attrstring);
        client1().client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
        changeClient().client.putnodes(n2->nodeHandle(), move(tc.nn), nullptr, ++next_request_tag);
    }

    void remote_renamed_move(std::string nodepath, std::string newparentpath, string newname, bool updatemodel, bool reportaction)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (updatemodel)
        {
            remoteModel.emulate_rename_copy(nodepath, newparentpath, newname);
        }

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote rename + move " << n1->displaypath() << " to " << n2->displaypath() << " as " << newname;

        error e = changeClient().client.rename(n1, n2, SYNCDEL_NONE, NodeHandle(), newname.c_str(), nullptr);
        EXPECT_EQ(e, API_OK);
    }

    void remote_delete(std::string nodepath, bool updatemodel, bool reportaction, bool mightNotExist)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        if (mightNotExist && !n) return;  // eg when checking to remove an item that is a move target but there isn't one

        ASSERT_TRUE(!!n);

        if (reportaction) out() << name() << " action: remote delete " << n->displaypath();

        if (updatemodel) remoteModel.emulate_delete(nodepath);

        auto e = changeClient().client.unlink(n, false, ++next_request_tag);
        ASSERT_TRUE(!e);
    }

    fs::path fixSeparators(std::string p)
    {
        for (auto& c : p)
            if (c == '/')
                c = fs::path::preferred_separator;
        return fs::u8path(p);
    }

    void local_rename(std::string path, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (deleteTargetFirst) local_delete(parentpath(path) + "/" + newname, updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) localModel.emulate_rename(path, newname);

        fs::path p1(localTestBasePath());
        p1 /= fixSeparators(path);
        fs::path p2 = p1.parent_path() / newname;

        if (reportaction) out() << name() << " action: local rename " << p1 << " to " << p2;

        std::error_code ec;
        for (int i = 0; i < 5; ++i)
        {
            fs::rename(p1, p2, ec);
            if (!ec) break;
            WaitMillisec(100);
        }
        ASSERT_TRUE(!ec) << "local_rename " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_move(std::string from, std::string to, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (deleteTargetFirst) local_delete(to + "/" + leafname(from), updatemodel, reportaction, true);

        if (updatemodel) localModel.emulate_move(from, to);

        fs::path p1(localTestBasePath());
        fs::path p2(localTestBasePath());
        p1 /= fixSeparators(from);
        p2 /= fixSeparators(to);
        p2 /= p1.filename();  // non-existing file in existing directory case

        if (reportaction) out() << name() << " action: local move " << p1 << " to " << p2;

        std::error_code ec;
        fs::rename(p1, p2, ec);
        if (ec)
        {
            fs::remove_all(p2, ec);
            fs::rename(p1, p2, ec);
        }
        ASSERT_TRUE(!ec) << "local_move " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_copy(std::string from, std::string to, bool updatemodel, bool reportaction)
    {
        if (updatemodel) localModel.emulate_copy(from, to);

        fs::path p1(localTestBasePath());
        fs::path p2(localTestBasePath());
        p1 /= fixSeparators(from);
        p2 /= fixSeparators(to);

        if (reportaction) out() << name() << " action: local copy " << p1 << " to " << p2;

        std::error_code ec;
        fs::copy(p1, p2, ec);
        ASSERT_TRUE(!ec) << "local_copy " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_delete(std::string path, bool updatemodel, bool reportaction, bool mightNotExist)
    {
        fs::path p(localTestBasePath());
        p /= fixSeparators(path);

        if (mightNotExist && !fs::exists(p)) return;

        if (reportaction) out() << name() << " action: local_delete " << p;

        std::error_code ec;
        fs::remove_all(p, ec);
        ASSERT_TRUE(!ec) << "local_delete " << p << " failed: " << ec.message();
        if (updatemodel) localModel.emulate_delete(path);
    }

    void source_rename(std::string nodepath, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (up) local_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
        else remote_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
    }

    void source_move(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (up) local_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
        else remote_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
    }

    void source_copy(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction)
    {
        if (up) local_copy(nodepath, newparentpath, updatemodel, reportaction);
        else remote_copy(nodepath, newparentpath, updatemodel, reportaction);
    }

    void source_delete(std::string nodepath, bool updatemodel, bool reportaction = false)
    {
        if (up) local_delete(nodepath, updatemodel, reportaction, false);
        else remote_delete(nodepath, updatemodel, reportaction, false);
    }

    void fileMayDiffer(std::string filepath)
    {
        fs::path p(localTestBasePath());
        p /= fixSeparators(filepath);

        client1().localFSFilesThatMayDiffer.insert(p);
        out() << "File may differ: " << p;
    }

    // Two-way sync has been started and is stable.  Now perform the test action

    enum ModifyStage { Prepare, MainAction };

    void PrintLocalTree(fs::path p)
    {
        out() << p;
        if (fs::is_directory(p))
        {
            for (auto i = fs::directory_iterator(p); i != fs::directory_iterator(); ++i)
            {
                PrintLocalTree(*i);
            }
        }
    }

    void PrintLocalTree(const LocalNode& node)
    {
        out() << node.getLocalPath().toPath();

        if (node.type == FILENODE) return;

        for (const auto& childIt : node.children)
        {
            PrintLocalTree(*childIt.second);
        }
    }

    void PrintRemoteTree(Node* n, string prefix = "")
    {
        prefix += string("/") + n->displayname();
        out() << prefix;
        if (n->type == FILENODE) return;
        for (auto& c : n->children)
        {
            PrintRemoteTree(c, prefix);
        }
    }

    void PrintModelTree(Model::ModelNode* n, string prefix = "")
    {
        prefix += string("/") + n->name;
        out() << prefix;
        if (n->type == Model::ModelNode::file) return;
        for (auto& c : n->kids)
        {
            PrintModelTree(c.get(), prefix);
        }
    }

    void Modify(ModifyStage stage)
    {
        bool prep = stage == Prepare;
        bool act = stage == MainAction;

        if (prep) out() << "Preparing action ";
        if (act) out() << "Executing action ";

        if (prep && printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem initial state ----";
            PrintLocalTree(fs::path(localTestBasePath()));

            if (auto* sync = client1().syncByBackupId(backupId))
            {
                out() << " ---- local node tree initial state ----";
                PrintLocalTree(*sync->localroot);
            }

            out() << " ---- remote node tree initial state ----";
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
        }

        switch (action)
        {
        case action_rename:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_rename("f/f_0/file0_f_0", "file0_f_0_renamed", shouldUpdateModel(), true, true);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_rename("f/f_0/file0_f_0", "file0_f_0_renamed");
                    }
                }
                else
                {
                    source_rename("f/f_0", "f_0_renamed", shouldUpdateModel(), true, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_rename("f/f_0", "f_0_renamed");
                    }
                }
            }
            break;

        case action_moveWithinSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("f/f_1/file0_f_1", "f/f_0", shouldUpdateModel(), true, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_move("f/f_1/file0_f_1", "f/f_0");
                    }
                }
                else
                {
                    source_move("f/f_1", "f/f_0", shouldUpdateModel(), true, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_move("f/f_1", "f/f_0");
                    }
                }
            }
            break;

        case action_moveOutOfSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("f/f_0/file0_f_0", "outside", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0/file0_f_0");
                    }
                }
                else
                {
                    source_move("f/f_0", "outside", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0");
                    }
                }
            }
            break;

        case action_moveIntoSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("outside/file0_outside", "f/f_0", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_copy("outside/file0_outside", "f/f_0");
                    }
                }
                else
                {
                    source_move("outside", "f/f_0", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0/outside");
                        destinationModel().emulate_copy("outside", "f/f_0");
                    }
                }
            }
            break;

        case action_delete:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_delete("f/f_0/file0_f_0", shouldUpdateModel(), true);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0/file0_f_0");
                    }
                }
                else
                {
                    source_delete("f/f_0", shouldUpdateModel(), true);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0");
                    }
                }
            }
            break;

        default: ASSERT_TRUE(false);
        }
    }

    void CheckSetup(State&, bool initial)
    {
        if (!initial && printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem before change ----";
            PrintLocalTree(fs::path(localTestBasePath()));

            if (auto* sync = client1().syncByBackupId(backupId))
            {
                out() << " ---- local node tree before change ----";
                PrintLocalTree(*sync->localroot);
            }

            out() << " ---- remote node tree before change ----";
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
        }

        if (!initial) out() << "Checking setup state (should be no changes in twoway sync source): "<< name();

        // confirm source is unchanged after setup  (Two-way is not sending changes to the wrong side)
        bool localfs = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALFS, true); // todo: later enable debris checks
        bool localnode = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALNODE, true); // todo: later enable debris checks
        bool remote = client1().confirmModel(backupId, remoteModel.findnode("f"), StandardClient::CONFIRM_REMOTE, true); // todo: later enable debris checks
        EXPECT_EQ(localfs, localnode);
        EXPECT_EQ(localnode, remote);
        EXPECT_TRUE(localfs && localnode && remote) << " failed in " << name();
    }


    // Two-way sync is stable again after the change.  Check the results.
    bool finalResult = false;
    void CheckResult(State&)
    {
        Sync* sync = client1().syncByBackupId(backupId);

        if (printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem after sync of change ----";
            PrintLocalTree(fs::path(localTestBasePath()));

            if (sync)
            {
                out() << " ---- local node tree after sync of change ----";
                PrintLocalTree(*sync->localroot);
            }

            out() << " ---- remote node tree after sync of change ----";
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
            out() << " ---- expected sync destination (model) ----";
            PrintModelTree(destinationModel().findnode("f"));
        }

        out() << "Checking twoway sync "<< name();

        if (shouldDisableSync())
        {
            bool lfs = client1().confirmModel(backupId, localModel.findnode("f"), localSyncRootPath(), true);
            bool rnt = client1().confirmModel(backupId, remoteModel.findnode("f"), remoteSyncRoot());

            EXPECT_EQ(sync, nullptr) << "Sync isn't disabled: " << name();
            EXPECT_TRUE(lfs) << "Couldn't confirm LFS: " << name();
            EXPECT_TRUE(rnt) << "Couldn't confirm RNT: " << name();

            finalResult = sync == nullptr;
            finalResult &= lfs;
            finalResult &= rnt;
        }
        else
        {
            EXPECT_NE(sync, (Sync*)nullptr);
            EXPECT_TRUE(sync && sync->state() == SYNC_ACTIVE);

            bool localfs = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALFS, true); // todo: later enable debris checks
            bool localnode = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALNODE, true); // todo: later enable debris checks
            bool remote = client1().confirmModel(backupId, remoteModel.findnode("f"), StandardClient::CONFIRM_REMOTE, true); // todo: later enable debris checks
            EXPECT_EQ(localfs, localnode);
            EXPECT_EQ(localnode, remote);
            EXPECT_TRUE(localfs && localnode && remote) << " failed in " << name();

            finalResult = localfs && localnode && remote && sync && sync->state() == SYNC_ACTIVE;
        }

    }
};

void CatchupClients(StandardClient* c1, StandardClient* c2 = nullptr, StandardClient* c3 = nullptr)
{
    out() << "Catching up";
    auto pb1 = newPromiseBoolSP();
    auto pb2 = newPromiseBoolSP();
    auto pb3 = newPromiseBoolSP();
    if (c1) c1->catchup(pb1);
    if (c2) c2->catchup(pb2);
    if (c3) c3->catchup(pb3);
    ASSERT_TRUE((!c1 || pb1->get_future().get()) &&
                (!c2 || pb2->get_future().get()) &&
                (!c3 || pb3->get_future().get()));
    out() << "Caught up";
}

void PrepareForSync(StandardClient& client)
{
    auto local = client.fsBasePath / "twoway" / "initial";

    fs::create_directories(local);

    ASSERT_TRUE(buildLocalFolders(local, "f", 2, 2, 2));
    ASSERT_TRUE(buildLocalFolders(local, "outside", 2, 1, 1));

    constexpr auto delta = std::chrono::seconds(3600);

    ASSERT_TRUE(createDataFile(local / "f" / "file_older_1", "file_older_1", -delta));
    ASSERT_TRUE(createDataFile(local / "f" / "file_older_2", "file_older_2", -delta));
    ASSERT_TRUE(createDataFile(local / "f" / "file_newer_1", "file_newer_1", delta));
    ASSERT_TRUE(createDataFile(local / "f" / "file_newer_2", "file_newer_2", delta));

    auto* remote = client.drillchildnodebyname(client.gettestbasenode(), "twoway");
    ASSERT_NE(remote, nullptr);

    ASSERT_TRUE(client.uploadFolderTree(local, remote));
    ASSERT_TRUE(client.uploadFilesInTree(local, remote));
}

bool WaitForRemoteMatch(map<string, TwoWaySyncSymmetryCase>& testcases,
                        chrono::seconds timeout)
{
    auto total = std::chrono::milliseconds(0);
    constexpr auto sleepIncrement = std::chrono::milliseconds(500);

    do
    {
        auto i = testcases.begin();
        auto j = testcases.end();

        for ( ; i != j; ++i)
        {
            auto& testcase = i->second;

            if (testcase.pauseDuringAction) continue;

            auto& client = testcase.client1();
            auto& id = testcase.backupId;
            auto& model = testcase.remoteModel;

            if (!client.match(id, model.findnode("f")))
            {
                out() << "Cloud/model misatch: " << testcase.name();
                break;
            }
        }

        if (i == j)
        {
            out() << "Cloud/model matched.";
            return true;
        }

        out() << "Waiting for cloud/model match...";

        std::this_thread::sleep_for(sleepIncrement);
        total += sleepIncrement;
    }
    while (total < timeout);

    out() << "Timed out waiting for cloud/model match.";

    return false;
}

TEST_F(SyncTest, TwoWay_Highlevel_Symmetries)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();

    StandardClient clientA2(localtestroot, "clientA2");

    ASSERT_TRUE(clientA2.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "twoway", 0, 0, true));

    PrepareForSync(clientA2);

    StandardClient clientA1Steady(localtestroot, "clientA1S");
    StandardClient clientA1Resume(localtestroot, "clientA1R");
    ASSERT_TRUE(clientA1Steady.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    ASSERT_TRUE(clientA1Resume.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    fs::create_directory(clientA1Steady.fsBasePath / fs::u8path("twoway"));
    fs::create_directory(clientA1Resume.fsBasePath / fs::u8path("twoway"));
    fs::create_directory(clientA2.fsBasePath / fs::u8path("twoway"));

    TwoWaySyncSymmetryCase::State allstate(clientA1Steady, clientA1Resume, clientA2);
    allstate.localBaseFolderSteady = clientA1Steady.fsBasePath / fs::u8path("twoway");
    allstate.localBaseFolderResume = clientA1Resume.fsBasePath / fs::u8path("twoway");

    std::map<std::string, TwoWaySyncSymmetryCase> cases;

    static set<string> tests = {
    }; // tests

    for (int syncType = TwoWaySyncSymmetryCase::type_numTypes; syncType--; )
    {
        //if (syncType != TwoWaySyncSymmetryCase::type_backupSync) continue;

        for (int selfChange = 0; selfChange < 2; ++selfChange)
        {
            //if (!selfChange) continue;

            for (int up = 0; up < 2; ++up)
            {
                //if (!up) continue;

                for (int action = 0; action < (int)TwoWaySyncSymmetryCase::action_numactions; ++action)
                {
                    //if (action != TwoWaySyncSymmetryCase::action_rename) continue;

                    for (int file = 0; file < 2; ++file)
                    {
                        //if (!file) continue;

                        for (int isExternal = 0; isExternal < 2; ++isExternal)
                        {
                            if (isExternal && syncType != TwoWaySyncSymmetryCase::type_backupSync)
                            {
                                continue;
                            }

                            for (int pauseDuringAction = 0; pauseDuringAction < 2; ++pauseDuringAction)
                            {
                                //if (pauseDuringAction) continue;

                                // we can't make changes if the client is not running
                                if (pauseDuringAction && selfChange) continue;

                                TwoWaySyncSymmetryCase testcase(allstate);
                                testcase.syncType = TwoWaySyncSymmetryCase::SyncType(syncType);
                                testcase.selfChange = selfChange != 0;
                                testcase.up = up;
                                testcase.action = TwoWaySyncSymmetryCase::Action(action);
                                testcase.file = file;
                                testcase.isExternal = isExternal;
                                testcase.pauseDuringAction = pauseDuringAction;
                                testcase.printTreesBeforeAndAfter = !tests.empty();

                                if (tests.empty() || tests.count(testcase.name()) > 0)
                                {
                                    auto name = testcase.name();
                                    cases.emplace(name, move(testcase));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    out() << "Creating initial local files/folders for " << cases.size() << " sync test cases";
    for (auto& testcase : cases)
    {
        testcase.second.SetupForSync();
    }

    // set up sync for A1, it should build matching cloud files/folders as the test cases add local files/folders
    handle backupId1 = clientA1Steady.setupSync_mainthread("twoway", "twoway");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA1Resume.setupSync_mainthread("twoway", "twoway");
    ASSERT_NE(backupId2, UNDEF);
    assert(allstate.localBaseFolderSteady == clientA1Steady.syncSet(backupId1).localpath);
    assert(allstate.localBaseFolderResume == clientA1Resume.syncSet(backupId2).localpath);

    out() << "Full-sync all test folders to the cloud for setup";
    waitonsyncs(std::chrono::seconds(10), &clientA1Steady, &clientA1Resume);
    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);
    waitonsyncs(std::chrono::seconds(20), &clientA1Steady, &clientA1Resume);

    out() << "Stopping full-sync";
    auto removeSyncByBackupId =
      [](StandardClient& sc, handle backupId)
      {
          bool removed = false;

          sc.client.syncs.removeSelectedSyncs(
            [&](SyncConfig& config, Sync*)
            {
                const bool matched = config.getBackupId() == backupId;
                removed |= matched;
                return matched;
            });

          return removed;
      };

    future<bool> fb1 = clientA1Steady.thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb) { pb->set_value(removeSyncByBackupId(sc, backupId1)); });
    future<bool> fb2 = clientA1Resume.thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb) { pb->set_value(removeSyncByBackupId(sc, backupId2)); });
    ASSERT_TRUE(waitonresults(&fb1, &fb2));

    out() << "Setting up each sub-test's Two-way sync of 'f'";
    for (auto& testcase : cases)
    {
        testcase.second.SetupTwoWaySync();
    }

    out() << "Letting all " << cases.size() << " Two-way syncs run";
    waitonsyncs(std::chrono::seconds(10), &clientA1Steady, &clientA1Resume);

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);
    waitonsyncs(std::chrono::seconds(10), &clientA1Steady, &clientA1Resume);

    out() << "Checking intial state";
    for (auto& testcase : cases)
    {
        testcase.second.CheckSetup(allstate, true);
    }

    // make changes in destination to set up test
    for (auto& testcase : cases)
    {
        testcase.second.Modify(TwoWaySyncSymmetryCase::Prepare);
    }

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Letting all " << cases.size() << " Two-way syncs run";
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Checking Two-way source is unchanged";
    for (auto& testcase : cases)
    {
        testcase.second.CheckSetup(allstate, false);
    }


    int paused = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.pauseDuringAction)
        {
            testcase.second.PauseTwoWaySync();
            ++paused;
        }
    }

    // save session and local log out A1R to set up for resume
    string session;
    clientA1Resume.client.dumpsession(session);
    clientA1Resume.localLogout();

    if (paused)
    {
        out() << "Paused " << paused << " Two-way syncs";
        WaitMillisec(1000);
    }

    out() << "Performing action ";
    for (auto& testcase : cases)
    {
        testcase.second.Modify(TwoWaySyncSymmetryCase::MainAction);
    }
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA2);   // leave out clientA1Resume as it's 'paused' (locallogout'd) for now
    CatchupClients(&clientA1Steady, &clientA2);
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA2);   // leave out clientA1Resume as it's 'paused' (locallogout'd) for now

    // resume A1R session (with sync), see if A2 nodes and localnodes get in sync again
    ASSERT_TRUE(clientA1Resume.login_fetchnodes(session));
    ASSERT_EQ(clientA1Resume.basefolderhandle, clientA2.basefolderhandle);

    int resumed = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.pauseDuringAction)
        {
            testcase.second.ResumeTwoWaySync();
            ++resumed;
        }
    }
    if (resumed)
    {
        out() << "Resumed " << resumed << " Two-way syncs";
        WaitMillisec(3000);
    }

    out() << "Waiting for remote changes to make it to clients...";
    EXPECT_TRUE(WaitForRemoteMatch(cases, chrono::seconds(16)));

    out() << "Letting all " << cases.size() << " Two-way syncs run";

    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Checking local and remote state in each sub-test";

    for (auto& testcase : cases)
    {
        testcase.second.CheckResult(allstate);
    }
    int succeeded = 0, failed = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.finalResult) ++succeeded;
        else
        {
            out() << "failed: " << testcase.second.name();
            ++failed;
        }
    }
    out() << "Succeeded: " << succeeded << " Failed: " << failed;

    // Clear tree-state cache.
    {
        StandardClient cC(localtestroot, "cC");
        ASSERT_TRUE(cC.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    }
}

TEST_F(SyncTest, MoveExistingIntoNewDirectoryWhilePaused)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = chrono::seconds(4);

    Model model;
    fs::path root;
    string session;
    handle id;

    // Initial setup.
    {
        StandardClient c(TESTROOT, "c");

        // Log in client.
        ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

        // Add and start sync.
        id = c.setupSync_mainthread("s", "s");
        ASSERT_NE(id, UNDEF);

        // Squirrel away for later use.
        root = c.syncSet(id).localpath.u8string();

        // Populate filesystem.
        model.addfolder("a");
        model.addfolder("c");
        model.generate(root);

        // Wait for initial sync to complete.
        waitonsyncs(TIMEOUT, &c);

        // Make sure everything arrived safely.
        ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

        // Save the session so we can resume later.
        c.client.dumpsession(session);

        // Log out client, taking care to keep caches.
        c.localLogout();
    }

    StandardClient c(TESTROOT, "c");

    // Add a new hierarchy to be scanned.
    model.addfolder("b");
    model.generate(root);

    // Move c under b.
    fs::rename(root / "c", root / "b" / "c");

    // Update the model.
    model.movenode("c", "b");

    // Log in client resuming prior session.
    ASSERT_TRUE(c.login_fetchnodes(session));

    // Wait for the sync to catch up.
    waitonsyncs(TIMEOUT, &c);

    // Were the changes propagated?
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

// Useful predicates.
const auto SyncDisabled = [](handle id) {
    return [id](StandardClient& client) {
        return client.syncByBackupId(id) == nullptr;
    };
};

const auto SyncMonitoring = [](handle id) {
    return [id](StandardClient& client) {
        const auto* sync = client.syncByBackupId(id);
        return sync && sync->isBackupMonitoring();
    };
};

TEST_F(SyncTest, ForeignChangesInTheCloudDisablesMonitoringBackup)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    const auto id = c.setupSync_mainthread("s", "s", true);
    ASSERT_NE(id, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure we're in monitoring mode.
    ASSERT_TRUE(c.waitFor(SyncMonitoring(id), TIMEOUT));

    // Make a (foreign) change to the cloud.
    {
        StandardClient cu(TESTROOT, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log in client.
        ASSERT_TRUE(cu.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // Create a directory.
        vector<NewNode> node(1);

        cu.client.putnodes_prepareOneFolder(&node[0], "d");

        ASSERT_TRUE(cu.putnodes(c.syncSet(id).h, std::move(node)));
    }

    // Give our sync some time to process remote changes.
    waitonsyncs(TIMEOUT, &c);

    // Wait for the sync to be disabled.
    ASSERT_TRUE(c.waitFor(SyncDisabled(id), TIMEOUT));

    // Has the sync failed?
    {
        SyncConfig config = c.syncConfigByBackupID(id);

        ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
        ASSERT_EQ(config.mEnabled, false);
        ASSERT_EQ(config.mError, BACKUP_MODIFIED);
    }
}

class BackupClient
  : public StandardClient
{
public:
    BackupClient(const fs::path& basePath, const string& name)
      : StandardClient(basePath, name)
      , mOnFileAdded()
    {
    }

    void file_added(File* file) override
    {
        StandardClient::file_added(file);

        if (mOnFileAdded) mOnFileAdded(*file);
    }

    using FileAddedCallback = std::function<void(File&)>;

    FileAddedCallback mOnFileAdded;
}; // Client

TEST_F(SyncTest, MonitoringExternalBackupRestoresInMirroringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    // Model.
    Model m;

    // Sync Root Handle.
    NodeHandle rootHandle;

    // Session ID.
    string sessionID;

    // Sync Backup ID.
    handle id;

    {
        StandardClient cb(TESTROOT, "cb");

        // Log callbacks.
        cb.logcb = true;

        // Log in client.
        ASSERT_TRUE(cb.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

        // Create some files to synchronize.
        m.addfile("d/f");
        m.addfile("f");
        m.generate(cb.fsBasePath / "s");

        // Add and start sync.
        {
            // Generate drive ID.
            auto driveID = cb.client.generateDriveId();

            // Write drive ID.
            auto drivePath = cb.fsBasePath.u8string();
            auto result = cb.client.writeDriveId(drivePath.c_str(), driveID);
            ASSERT_EQ(result, API_OK);

            // Add sync.
            id = cb.backupAdd_mainthread("", "s", "s");
            ASSERT_NE(id, UNDEF);
        }

        // Wait for sync to complete.
        waitonsyncs(TIMEOUT, &cb);

        // Make sure everything made it to the cloud.
        ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));

        // Wait for sync to transition to monitoring mode.
        ASSERT_TRUE(cb.waitFor(SyncMonitoring(id), TIMEOUT));

        // Get our hands on the sync's root handle.
        rootHandle = cb.syncSet(id).h;

        // Record this client's session.
        cb.client.dumpsession(sessionID);

        // Log out the client.
        cb.localLogout();
    }

    StandardClient cb(TESTROOT, "cb");

    cb.logcb = true;

    // Log in client.
    ASSERT_TRUE(cb.login_fetchnodes(sessionID));

    // Make a change in the cloud.
    {
        vector<NewNode> node(1);

        cb.client.putnodes_prepareOneFolder(&node[0], "g");

        ASSERT_TRUE(cb.putnodes(rootHandle, std::move(node)));
    }

    // Restore the backup sync.
    ASSERT_TRUE(cb.backupOpenDrive(cb.fsBasePath));

    // Re-enable the sync.
    ASSERT_TRUE(cb.enableSyncByBackupId(id));

    // Wait for the mirror to complete.
    waitonsyncs(TIMEOUT, &cb);

    // Cloud should mirror the local disk.
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));
}

TEST_F(SyncTest, MonitoringExternalBackupResumesInMirroringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient cb(TESTROOT, "cb");

    // Log callbacks.
    cb.logcb = true;

    // Log in client.
    ASSERT_TRUE(cb.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Create some files to be synchronized.
    Model m;

    m.addfile("d/f");
    m.addfile("f");
    m.generate(cb.fsBasePath / "s");

    // Add and start sync.
    auto id = UNDEF;

    {
        // Generate drive ID.
        auto driveID = cb.client.generateDriveId();

        // Write drive ID.
        auto drivePath = cb.fsBasePath.u8string();
        auto result = cb.client.writeDriveId(drivePath.c_str(), driveID);
        ASSERT_EQ(result, API_OK);

        // Add sync.
        id = cb.backupAdd_mainthread("", "s", "s");
        ASSERT_NE(id, UNDEF);
    }

    // Wait for the mirror to complete.
    waitonsyncs(TIMEOUT, &cb);

    // Make sure everything arrived safe and sound.
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));

    // Wait for transition to monitoring mode.
    ASSERT_TRUE(cb.waitFor(SyncMonitoring(id), TIMEOUT));

    // Disable the sync.
    ASSERT_TRUE(cb.disableSync(id, NO_SYNC_ERROR, true));

    // Make sure the sync's config is as we expect.
    {
        auto config = cb.syncConfigByBackupID(id);

        // Backup should remain in monitoring mode.
        ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);

        // Disabled external backups are always considered "user-disabled."
        // That is, the user must consciously decide to resume these syncs.
        ASSERT_EQ(config.mEnabled, false);
    }

    // Make a change in the cloud.
    {
        vector<NewNode> node(1);

        cb.client.putnodes_prepareOneFolder(&node[0], "g");

        auto rootHandle = cb.syncSet(id).h;
        ASSERT_TRUE(cb.putnodes(rootHandle, std::move(node)));
    }

    // Re-enable the sync.
    ASSERT_TRUE(cb.enableSyncByBackupId(id));

    // Wait for the mirror to complete.
    waitonsyncs(TIMEOUT, &cb);

    // Cloud should mirror the disk.
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));
}

TEST_F(SyncTest, MirroringInternalBackupResumesInMirroringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    // Session ID.
    string sessionID;

    // Sync Backup ID.
    handle id;

    // Sync Root Handle.
    NodeHandle rootHandle;

    // "Foreign" client.
    StandardClient cf(TESTROOT, "cf");

    // Model.
    Model m;

    // Log callbacks.
    cf.logcb = true;

    // Log client in.
    ASSERT_TRUE(cf.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Check manual resume.
    {
        BackupClient cb(TESTROOT, "cb");

        // Log callbacks.
        cb.logcb = true;

        // Log client in.
        ASSERT_TRUE(cb.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // Set upload throttle.
        //
        // This is so that we can disable the sync before it transitions
        // to the monitoring state.
        cb.client.setmaxuploadspeed(1);

        // Give the sync something to backup.
        m.addfile("d/f", randomData(16384));
        m.addfile("f", randomData(16384));
        m.generate(cb.fsBasePath / "s");

        // Disable the sync when it starts uploading a file.
        cb.mOnFileAdded = [&cb](File& file) {
            // Get our hands on the local node.
            auto* node = dynamic_cast<LocalNode*>(&file);
            if (!node) return;

            // Get our hands on the sync.
            auto& sync = *node->sync;

            // Make sure the sync's in mirroring mode.
            ASSERT_TRUE(sync.isBackupAndMirroring());

            // Disable the sync.
            sync.changestate(SYNC_DISABLED, NO_SYNC_ERROR, true, true);

            // Mimic disableSelectedSyncs(...).
            cb.client.syncactivity = true;

            // Callback's done its job.
            cb.mOnFileAdded = nullptr;
        };

        // Add and start sync.
        id = cb.setupSync_mainthread("s", "s", true);
        ASSERT_NE(id, UNDEF);

        // Let the sync mirror.
        waitonsyncs(TIMEOUT, &cb);

        // Make sure the sync's been disabled.
        ASSERT_FALSE(cb.syncByBackupId(id));

        // Make sure it's still in mirror mode.
        {
            auto config = cb.syncConfigByBackupID(id);

            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MIRROR);
            ASSERT_EQ(config.mEnabled, true);
            ASSERT_EQ(config.mError, NO_SYNC_ERROR);
        }

        // Get our hands on sync root's cloud handle.
        rootHandle = cb.syncSet(id).h;
        ASSERT_TRUE(!rootHandle.isUndef());

        // Make some changes to the cloud.
        vector<NewNode> node(1);

        cf.client.putnodes_prepareOneFolder(&node[0], "g");

        ASSERT_TRUE(cf.putnodes(rootHandle, std::move(node)));

        // Log out the client when we try and upload a file.
        std::promise<void> waiter;

        cb.mOnFileAdded = [&cb, &waiter](File& file) {
            // Get our hands on the local node.
            auto* node = dynamic_cast<LocalNode*>(&file);
            if (!node) return;

            // Make sure we're mirroring.
            ASSERT_TRUE(node->sync->isBackupAndMirroring());

            // Notify the waiter.
            waiter.set_value();

            // Callback's done its job.
            cb.mOnFileAdded = nullptr;
        };

        // Resume the backup.
        ASSERT_TRUE(cb.enableSyncByBackupId(id));

        // Wait for the sync to try and upload a file.
        waiter.get_future().get();

        // Save the session ID.
        cb.client.dumpsession(sessionID);

        // Log out the client.
        cb.localLogout();
    }

    // Create a couple new nodes.
    {
        vector<NewNode> nodes(2);

        cf.client.putnodes_prepareOneFolder(&nodes[0], "h0");
        cf.client.putnodes_prepareOneFolder(&nodes[1], "h1");

        ASSERT_TRUE(cf.putnodes(rootHandle, std::move(nodes)));
    }

    // Check automatic resume.
    StandardClient cb(TESTROOT, "cb");

    // Log callbacks.
    cb.logcb = true;

    // Log in client, resuming prior session.
    ASSERT_TRUE(cb.login_fetchnodes(sessionID));

    // Check config has been resumed.
    ASSERT_TRUE(cb.syncByBackupId(id));

    // Just let the sync mirror, Marge!
    waitonsyncs(TIMEOUT, &cb);

    // The cloud should match the local disk precisely.
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));
}

TEST_F(SyncTest, MonitoringInternalBackupResumesInMonitoringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(8);

    // Sync Backup ID.
    handle id;

    // Sync Root Handle.
    NodeHandle rootHandle;

    // Session ID.
    string sessionID;

    // "Foreign" client.
    StandardClient cf(TESTROOT, "cf");

    // Model.
    Model m;

    // Log callbacks.
    cf.logcb = true;

    // Log foreign client in.
    ASSERT_TRUE(cf.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Manual resume.
    {
        StandardClient cb(TESTROOT, "cb");

        // Log callbacks.
        cb.logcb = true;

        // Log in client.
        ASSERT_TRUE(cb.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // Give the sync something to mirror.
        m.addfile("d/f");
        m.addfile("f");
        m.generate(cb.fsBasePath / "s");

        // Add and start backup.
        id = cb.setupSync_mainthread("s", "s", true);
        ASSERT_NE(id, UNDEF);

        // Wait for the backup to complete.
        waitonsyncs(TIMEOUT, &cb);

        // Wait for transition to monitoring mode.
        ASSERT_TRUE(cb.waitFor(SyncMonitoring(id), TIMEOUT));

        // Disable the sync.
        ASSERT_TRUE(cb.disableSync(id, NO_SYNC_ERROR, true));

        // Make sure the sync was monitoring.
        {
            auto config = cb.syncConfigByBackupID(id);

            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
            ASSERT_EQ(config.mEnabled, true);
            ASSERT_EQ(config.mError, NO_SYNC_ERROR);
        }

        // Get our hands on the sync's root handle.
        rootHandle = cb.syncSet(id).h;

        // Make a remote change.
        //
        // This is so the backup will fail upon resume.
        {
            vector<NewNode> node(1);

            cf.client.putnodes_prepareOneFolder(&node[0], "g");

            ASSERT_TRUE(cf.putnodes(rootHandle, std::move(node)));
        }

        // Enable the backup.
        ASSERT_TRUE(cb.enableSyncByBackupId(id));

        // Give the sync some time to think.
        waitonsyncs(TIMEOUT, &cb);

        // Wait for the sync to be disabled.
        ASSERT_TRUE(cb.waitFor(SyncDisabled(id), TIMEOUT));

        // Make sure it's been disabled for the right reasons.
        {
            auto config = cb.syncConfigByBackupID(id);

            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
            ASSERT_EQ(config.mEnabled, false);
            ASSERT_EQ(config.mError, BACKUP_MODIFIED);
        }

        // Manually enable the sync.
        // It should come up in mirror mode.
        ASSERT_TRUE(cb.enableSyncByBackupId(id));

        // Let it bring the cloud in line.
        waitonsyncs(TIMEOUT, &cb);

        // Cloud should match the local disk precisely.
        ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));

        // Save the session ID.
        cb.client.dumpsession(sessionID);

        // Log out the client.
        cb.localLogout();
    }

    // Make a remote change.
    {
        vector<NewNode> node(1);

        cf.client.putnodes_prepareOneFolder(&node[0], "h");

        ASSERT_TRUE(cf.putnodes(rootHandle, std::move(node)));
    }

    // Automatic resume.
    StandardClient cb(TESTROOT, "cb");

    // Log callbacks.
    cb.logcb = true;

    // Log in the client.
    ASSERT_TRUE(cb.login_fetchnodes(sessionID));

    // Give the sync some time to think.
    waitonsyncs(TIMEOUT, &cb);

    // Wait for the sync to be disabled.
    ASSERT_TRUE(cb.waitFor(SyncDisabled(id), TIMEOUT));

    // Make sure it's been disabled for the right reasons.
    {
        auto config = cb.syncConfigByBackupID(id);

        ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
        ASSERT_EQ(config.mEnabled, false);
        ASSERT_EQ(config.mError, BACKUP_MODIFIED);
    }

    // Re-enable the sync.
    ASSERT_TRUE(cb.enableSyncByBackupId(id));

    // Wait for the sync to complete mirroring.
    waitonsyncs(TIMEOUT, &cb);

    // Cloud should mirror the disk.
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));
}

#endif
