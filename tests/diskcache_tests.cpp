#include "diskcache.h"
#include "filedigest.h"
#include <QtCore/QTemporaryDir>
#include <cstdlib>
#include <cstdio>

using namespace bn_graph_diff;
#define check(value) do { if (!(value)) { \
	std::fprintf(stderr, "disk cache check failed at line %d: %s\n", __LINE__, #value); \
	std::abort(); } } while (false)

int main()
{
	QTemporaryDir temporary;
	check(temporary.isValid());
	QDir root(temporary.path());
	QString identityFixture = root.filePath("identity.bin");
	{
		QFile file(identityFixture);
		check(file.open(QIODevice::WriteOnly));
		check(file.write("before", 6) == 6);
		file.close();
	}
	QFileInfo before(identityFixture);
	auto beforeDigest = sha256File(before);
	check(beforeDigest.has_value());
	{
		QFile file(identityFixture);
		check(file.open(QIODevice::WriteOnly));
		check(file.write("after!", 6) == 6);
		check(file.flush());
		check(file.setFileTime(before.lastModified(), QFileDevice::FileModificationTime));
		file.close();
	}
	QFileInfo after(identityFixture);
	check(before.size() == after.size());
	check(before.lastModified() == after.lastModified());
	check(sha256File(after) != beforeDigest);
	check(root.mkpath("matches") && root.mkpath("extractions") && root.mkpath("manifests"));
	auto write = [&](QString name, int size, qint64 time) {
		QString path = root.filePath(name);
		QFile file(path);
		check(file.open(QIODevice::WriteOnly));
		check(file.write(QByteArray(size, 'x')) == size);
		// Flush buffered bytes before setting fixture age; close() would otherwise
		// overwrite that timestamp and make the expected eviction order unstable.
		check(file.flush());
		check(file.setFileTime(QDateTime::fromMSecsSinceEpoch(time), QFileDevice::FileModificationTime));
		file.close();
		check(QFileInfo(path).lastModified().toMSecsSinceEpoch() == time);
		return path;
	};
	auto name = [](int number, const char* suffix) {
		return QString("%1").arg(number, 64, 16, QChar('0')) + suffix;
	};
	QString oldest = write("matches/" + name(1, ".json"), 40, 1000);
	QString middle = write("extractions/" + name(2, ".cache"), 40, 2000);
	QString newest = write("matches/" + name(3, ".json"), 40, 3000);
	QString oversized = write("matches/" + name(8, ".json"), 100, 5000);
	QString exportFile = write("matches/report.json", 100, 0);
	QString pending = write("matches/" + name(4, ".tmp"), 100, 0);
	QString manifest = write("manifests/" + name(5, ".json"), 100, 0);
	QString link = root.filePath("extractions/" + name(6, ".cache"));
	check(QFile::link(exportFile, link));
	auto retained = trimDiskCache(root.path(), 80);
	check(retained.bytes == 80 && retained.entries == 2 && retained.removed == 2);
	check(!QFile::exists(oversized));
	check(!QFile::exists(oldest) && QFile::exists(middle) && QFile::exists(newest));
	check(QFile::exists(exportFile) && QFile::exists(pending) && QFile::exists(manifest));
	check(QFileInfo(link).isSymLink());
	// A successful read promotes an older artifact above the newer one.
	QFile read(middle);
	check(read.open(QIODevice::ReadOnly));
	touchDiskCacheArtifact(read);
	read.close();
	retained = trimDiskCache(root.path(), 80, 1);
	check(retained.entries == 1 && QFile::exists(middle) && !QFile::exists(newest));
	retained = trimDiskCache(root.path(), 0);
	check(retained.bytes == 0 && retained.entries == 0 && !QFile::exists(middle));
	// Trimming never follows a cache-kind directory symlink.
	check(root.rename("matches", "saved-matches"));
	check(QFile::link(root.filePath("saved-matches"), root.filePath("matches")));
	QString hidden = write("saved-matches/" + name(7, ".json"), 100, 0);
	trimDiskCache(root.path(), 0);
	check(QFile::exists(hidden));
	return 0;
}
