#ifndef PRECOMPILE_H
#define PRECOMPILE_H

#include "GccArguments.h"
#include <Path.h>
#include <QObject>
#include <QList>
#include <clang-c/Index.h>

class Precompile : public QObject
{
    Q_OBJECT
public:
    static void create(const GccArguments &args,
                       const QByteArray &filePath,
                       const QHash<Path, quint64> &deps);
    static Precompile* precompiler(const GccArguments& args);
    static void cleanup();
    static QList<Precompile*> precompiles();    

    ~Precompile();

    void clear();
    void addData(const QByteArray& data);
    CXTranslationUnit precompile(const QList<QByteArray>& systemIncludes, CXIndex idx);

    QByteArray filePath() const;
    QByteArray headerFilePath() const;
    GccArguments arguments() const { return m_args; }
    void setDependencies(const QHash<Path, quint64> &deps) { m_dependencies = deps; }
    QHash<Path, quint64> dependencies() const { return m_dependencies; }
private:
    Precompile(const GccArguments& args, QObject* parent = 0);
    bool preprocessHeaders(QList<QByteArray> systemIncludes);

    QByteArray m_filePath, m_headerFilePath;
    QByteArray m_data;
    GccArguments m_args;

    QHash<Path, quint64> m_dependencies;

    static QHash<QByteArray, Precompile*> s_precompiles;
    static Path s_path;
};

#endif // PRECOMPILE_H
