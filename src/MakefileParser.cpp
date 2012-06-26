#include "MakefileParser.h"
#include <Log.h>
#include <List.h>
#include <stdio.h>
#include <RegExp.h>
#include <RTags.h>
#include "Rdm.h"
#include <QtCore>

#ifndef MAKE
#define MAKE "make"
#endif

class DirectoryTracker
{
public:
    DirectoryTracker();

    void init(const Path &path);
    void track(const ByteArray &line);

    const Path &path() const { return mPaths.back(); }

private:
    void enterDirectory(const ByteArray &dir);
    void leaveDirectory(const ByteArray &dir);

private:
    List<Path> mPaths;
};

DirectoryTracker::DirectoryTracker()
{
}

void DirectoryTracker::init(const Path &path)
{
    mPaths.push_back(path);
}

void DirectoryTracker::track(const ByteArray &line)
{
    // printf("Tracking %s\n", line.constData());
    static RegExp rx("make[^:]*: ([^ ]+) directory `([^']+)'");
    List<RegExp::Capture> captures;
    if (rx.indexIn(line.constData(), 0, &captures) != -1) {
        assert(captures.size() >= 3);
        if (captures.at(1).capture() == "Entering") {
            enterDirectory(captures.at(2).capture());
        } else if (captures.at(1).capture() == "Leaving") {
            leaveDirectory(captures.at(2).capture());
        } else {
            error("Invalid directory track: %s %s",
                  captures.at(1).capture().constData(),
                  captures.at(2).capture().constData());
        }
    }
}

void DirectoryTracker::enterDirectory(const ByteArray &dir)
{
    bool ok;
    Path newPath = Path::resolved(dir, path(), &ok);
    if (ok) {
        mPaths.push_back(newPath);
        debug("New directory resolved: %s", newPath.constData());
    } else {
        qFatal("Unable to resolve path %s (%s)", dir.constData(), path().constData());
    }
}

void DirectoryTracker::leaveDirectory(const ByteArray &dir)
{
    verboseDebug() << "leaveDirectory" << dir;
    // enter and leave share the same code for now
    mPaths.pop_back();
    // enterDirectory(dir);
}

MakefileParser::MakefileParser(const List<ByteArray> &extraFlags, Connection *conn)
    : QObject(), mProc(0), mTracker(new DirectoryTracker), mExtraFlags(extraFlags),
      mSourceCount(0), mPchCount(0), mConnection(conn)
{
}

MakefileParser::~MakefileParser()
{
    if (mProc) {
        mProc->kill();
        mProc->terminate();
        mProc->waitForFinished();
        delete mProc;
    }
    delete mTracker;
}

void MakefileParser::run(const Path &makefile, const List<ByteArray> &args)
{
    mMakefile = makefile;
    Q_ASSERT(!mProc);
    mProc = new QProcess(this);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!args.contains("-B")) {
        Path p = RTags::applicationDirPath();
#ifdef OS_Mac
        p += "/../makelib/libmakelib.dylib";
        p.resolve();
        environment.insert("DYLD_INSERT_LIBRARIES", p.constData());
#else
        p += "/../makelib/libmakelib.so";
        p.resolve();
        environment.insert("LD_PRELOAD", p.constData());
#endif
    }

    mProc->setProcessEnvironment(environment);

    connect(mProc, SIGNAL(readyReadStandardOutput()),
            this, SLOT(processMakeOutput()));
    connect(mProc, SIGNAL(readyReadStandardError()),
            this, SLOT(onReadyReadStandardError()));

    connect(mProc, SIGNAL(stateChanged(QProcess::ProcessState)),
            this, SLOT(onProcessStateChanged(QProcess::ProcessState)));

    connect(mProc, SIGNAL(error(QProcess::ProcessError)),
            this, SLOT(onError(QProcess::ProcessError)));
    connect(mProc, SIGNAL(finished(int)), this, SLOT(onDone()));

    mTracker->init(makefile.parentDir());
    warning(MAKE " -j1 -w -f %s -C %s\n",
            makefile.constData(), mTracker->path().constData());
    QStringList a;
    a << QLatin1String("-j1") << QLatin1String("-w")
      << QLatin1String("-f") << QString::fromLocal8Bit(makefile.constData(), makefile.size())
      << QLatin1String("-C") << QString::fromStdString(mTracker->path())
      << QLatin1String("AM_DEFAULT_VERBOSITY=1") << QLatin1String("VERBOSE=1");

    foreach(const ByteArray &arg, args) {
        a << QString::fromStdString(arg);
    }

    unlink("/tmp/makelib.log");
    mProc->start(QLatin1String(MAKE), a);
    mProc->waitForFinished();
}

bool MakefileParser::isDone() const
{
    return mProc && (mProc->state() == QProcess::NotRunning);
}

void MakefileParser::processMakeOutput()
{
    assert(mProc);
    mData += mProc->readAllStandardOutput().constData();

    // ### this could be more efficient
    int nextNewline = mData.indexOf('\n');
    while (nextNewline != -1) {
        processMakeLine(mData.left(nextNewline));
        mData = mData.mid(nextNewline + 1);
        nextNewline = mData.indexOf('\n');
    }
}

void MakefileParser::processMakeLine(const ByteArray &line)
{
    if (testLog(VerboseDebug))
        verboseDebug("%s", line.constData());
    GccArguments args;
    if (args.parse(line, mTracker->path())) {
        args.addFlags(mExtraFlags);
        if (args.type() == GccArguments::Pch) {
            ++mPchCount;
        } else {
            ++mSourceCount;
        }
        fileReady()(args);
    } else {
        mTracker->track(line);
    }
}

void MakefileParser::onError(QProcess::ProcessError err)
{
    error() << "Error" << int(err) << mProc->errorString().toStdString();
}

void MakefileParser::onProcessStateChanged(QProcess::ProcessState state)
{
    debug() << "process state changed " << state;
}

void MakefileParser::onReadyReadStandardError()
{
    debug() << "stderr" << mProc->readAllStandardError().constData();
}

List<ByteArray> MakefileParser::mapPchToInput(const List<ByteArray> &input) const
{
    List<ByteArray> output;
    Map<ByteArray, ByteArray>::const_iterator pchit;
    const Map<ByteArray, ByteArray>::const_iterator pchend = mPchs.end();
    foreach (const ByteArray &in, input) {
        pchit = mPchs.find(in);
        if (pchit != pchend)
            output.append(pchit->second);
    }
    return output;
}

void MakefileParser::setPch(const ByteArray &output, const ByteArray &input)
{
    mPchs[output] = input;
}

void MakefileParser::onDone()
{
    done()(this);
}
