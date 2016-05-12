/*
 * Copyright (c) 2014-2016 Meltytech, LLC
 * Author: Dan Dennedy <dan@dennedy.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mltxmlchecker.h"
#include "mltcontroller.h"
#include "shotcut_mlt_properties.h"
#include "util.h"
#include <QLocale>
#include <QDir>
#include <QCoreApplication>
#include <Logger.h>

bool isMltClass(const QStringRef& name)
{
    return name == "profile" || name == "producer" ||
           name == "filter" || name == "playlist" ||
           name == "tractor" || name == "track" ||
           name == "transition" || name == "consumer";
}

MltXmlChecker::MltXmlChecker()
    : m_needsGPU(false)
    , m_hasEffects(false)
    , m_isCorrected(false)
    , m_decimalPoint(QLocale::system().decimalPoint())
    , m_tempFile(QDir::tempPath().append("/shotcut-XXXXXX.mlt"))
    , m_hasComma(false)
    , m_hasPeriod(false)
    , m_numericValueChanged(false)
{
    LOG_DEBUG() << "decimal point" << m_decimalPoint;
    m_unlinkedFilesModel.setColumnCount(ColumnCount);
}

bool MltXmlChecker::check(const QString& fileName)
{
    LOG_DEBUG() << "begin";

    QFile file(fileName);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text) &&
            m_tempFile.open()) {
        m_tempFile.resize(0);
        m_basePath = QFileInfo(fileName).canonicalPath();
        m_xml.setDevice(&file);
        m_newXml.setDevice(&m_tempFile);
        if (m_xml.readNextStartElement()) {
            if (m_xml.name() == "mlt") {
                m_newXml.writeStartDocument();
                m_newXml.writeCharacters("\n");
                m_newXml.writeStartElement("mlt");
                foreach (QXmlStreamAttribute a, m_xml.attributes()) {
                    if (a.name().toString().toUpper() != "LC_NUMERIC")
                        m_newXml.writeAttribute(a);
                }
                readMlt();
                m_newXml.writeEndElement();
                m_newXml.writeEndDocument();
                m_isCorrected = m_isCorrected || (m_hasPeriod && m_hasComma && m_numericValueChanged);
            } else {
                m_xml.raiseError(QObject::tr("The file is not a MLT XML file."));
            }
        }
    }
    if (m_tempFile.isOpen()) {
        m_tempFile.close();
//        m_tempFile.open();
//        LOG_DEBUG() << m_tempFile.readAll().constData();
//        m_tempFile.close();
    }
    LOG_DEBUG() << "end";
    return m_xml.error() == QXmlStreamReader::NoError;
}

QString MltXmlChecker::errorString() const
{
    return m_xml.errorString();
}

void MltXmlChecker::readMlt()
{
    Q_ASSERT(m_xml.isStartElement() && m_xml.name() == "mlt");

    QString mlt_class;

    while (!m_xml.atEnd()) {
        switch (m_xml.readNext()) {
        case QXmlStreamReader::Characters:
            m_newXml.writeCharacters(m_xml.text().toString());
            break;
        case QXmlStreamReader::Comment:
            m_newXml.writeComment(m_xml.text().toString());
            break;
        case QXmlStreamReader::DTD:
            m_newXml.writeDTD(m_xml.text().toString());
            break;
        case QXmlStreamReader::EntityReference:
            m_newXml.writeEntityReference(m_xml.name().toString());
            break;
        case QXmlStreamReader::ProcessingInstruction:
            m_newXml.writeProcessingInstruction(m_xml.processingInstructionTarget().toString(), m_xml.processingInstructionData().toString());
            break;
        case QXmlStreamReader::StartDocument:
            m_newXml.writeStartDocument(m_xml.documentVersion().toString(), m_xml.isStandaloneDocument());
            break;
        case QXmlStreamReader::EndDocument:
            m_newXml.writeEndDocument();
            break;
        case QXmlStreamReader::StartElement: {
            const QString element = m_xml.name().toString();
            m_newXml.writeStartElement(m_xml.namespaceUri().toString(), element);
            if (isMltClass(m_xml.name())) {
                mlt_class = element;
            } else if (element == "property") {
                if (readMltService()) continue;
                if (checkNumericProperty()) continue;
                if ((mlt_class == "filter" || mlt_class == "transition" || mlt_class == "producer")) {
                    // Store a file reference for later checking.

                    //XXX This depends on mlt_service property appearing before resource.
                    if (m_service.name == "webvfx" && fixWebVfxPath()) continue;

                    if (readResourceProperty()) continue;
                    if (fixShotcutHashProperty()) continue;
                    if (readShotcutHashProperty()) continue;
                    if (fixShotcutDetailProperty()) continue;
                    if (fixShotcutCaptionProperty()) continue;
                    if (fixAudioIndexProperty()) continue;
                    if (fixVideoIndexProperty()) continue;
                }
            }
            checkInAndOutPoints(); // This also copies the attributes.
            break;
        }
        case QXmlStreamReader::EndElement:
            if (isMltClass(m_xml.name())) {

                if (!m_service.name.isEmpty() && !m_service.resource.filePath().isEmpty() && !m_service.resource.exists())
                if (!(m_service.name == "color" || m_service.name == "colour"))
                if (m_unlinkedFilesModel.findItems(m_service.resource.filePath(),
                        Qt::MatchFixedString | Qt::MatchCaseSensitive).isEmpty()) {
                    LOG_ERROR() << "file not found: " << m_service.resource.filePath();
                    QIcon icon(":/icons/oxygen/32x32/status/task-reject.png");
                    QStandardItem* item = new QStandardItem(icon, m_service.resource.filePath());
                    item->setToolTip(item->text());
                    item->setData(m_service.hash, ShotcutHashRole);
                    m_unlinkedFilesModel.appendRow(item);
                }

                mlt_class.clear();
                m_service.clear();
            }
            m_newXml.writeEndElement();
            break;
        default:
            break;
        }
    }
}

bool MltXmlChecker::readMltService()
{
    Q_ASSERT(m_xml.isStartElement() && m_xml.name() == "property");

    if (m_xml.attributes().value("name") == "mlt_service") {
        m_newXml.writeAttributes(m_xml.attributes());

        m_service.name = m_xml.readElementText();
        if (!MLT.isAudioFilter(m_service.name))
            m_hasEffects = true;
        if (m_service.name.startsWith("movit.") || m_service.name.startsWith("glsl."))
            m_needsGPU = true;
        m_newXml.writeCharacters(m_service.name);

        m_newXml.writeEndElement();
        return true;
    }
    return false;
}

void MltXmlChecker::checkInAndOutPoints()
{
    Q_ASSERT(m_xml.isStartElement());

    foreach (QXmlStreamAttribute a, m_xml.attributes()) {
        if (a.name() == "in" || a.name() == "out") {
            QString value = a.value().toString();
            if (checkNumericString(value)) {
                m_newXml.writeAttribute(a.name().toString(), value);
                continue;
            }
        }
        m_newXml.writeAttribute(a);
    }
}

bool MltXmlChecker::checkNumericString(QString& value)
{
    if (!m_hasComma)
        m_hasComma = value.contains(',');
    if (!m_hasPeriod)
        m_hasPeriod = value.contains('.');
    if (!value.contains(m_decimalPoint) &&
            (value.contains('.') || value.contains(','))) {
        value.replace(',', m_decimalPoint);
        value.replace('.', m_decimalPoint);
        m_numericValueChanged = true;
        return true;
    }
    return false;
}

bool MltXmlChecker::checkNumericProperty()
{
    Q_ASSERT(m_xml.isStartElement() && m_xml.name() == "property");

    QStringRef name = m_xml.attributes().value("name");
    if (name == "length" || name == "geometry") {
        m_newXml.writeAttributes(m_xml.attributes());

        QString value = m_xml.readElementText();
        checkNumericString(value);
        m_newXml.writeCharacters(value);

        m_newXml.writeEndElement();
        return true;
    }
    return false;
}

bool MltXmlChecker::fixWebVfxPath()
{
    Q_ASSERT(m_xml.isStartElement() && m_xml.name() == "property");

    if (m_xml.attributes().value("name") == "resource") {
        m_newXml.writeAttributes(m_xml.attributes());

        QString resource = m_xml.readElementText();

        // The path, if absolute, should start with the Shotcut executable path.
        QFileInfo fi(resource);
        if (fi.isAbsolute()) {
            QDir appPath(QCoreApplication::applicationDirPath());

#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
            // Leave the bin directory on Linux.
            appPath.cdUp();
#endif
            if (!resource.startsWith(appPath.path())) {
                // Locate "share/shotcut" and replace the front of it with appPath.
                int i = resource.indexOf("/share/shotcut/");
                if (i >= 0) {
                    resource.replace(0, i, appPath.path());
                    m_isCorrected = true;
                }
            }
        }
        m_newXml.writeCharacters(resource);

        m_newXml.writeEndElement();
        return true;
    }
    return false;
}

bool MltXmlChecker::readResourceProperty()
{
    const QStringRef name = m_xml.attributes().value("name");
    if (name == "resource" || name == "src" || name == "filename"
            || name == "luma" || name == "luma.resource" || name == "composite.luma"
            || name == "producer.resource") {

        m_newXml.writeAttributes(m_xml.attributes());
        QString text = m_xml.readElementText();

        // Save the resource name for later check for unlinked files.
        m_service.resource.setFile(text);
        if (m_service.resource.isRelative())
            m_service.resource.setFile(m_basePath, m_service.resource.filePath());

        // Replace unlinked files if model is populated with replacements.
        if (!fixUnlinkedFile())
            m_newXml.writeCharacters(text);

        m_newXml.writeEndElement();
        return true;
    }
    return false;
}

bool MltXmlChecker::readShotcutHashProperty()
{
    if (m_xml.attributes().value("name") == kShotcutHashProperty) {
        m_newXml.writeAttributes(m_xml.attributes());
        m_service.hash = m_xml.readElementText();
        m_newXml.writeCharacters(m_service.hash);
        m_newXml.writeEndElement();
        return true;
    }
    return false;
}

bool MltXmlChecker::fixUnlinkedFile()
{
    for (int row = 0; row < m_unlinkedFilesModel.rowCount(); ++row) {
        const QStandardItem* replacement = m_unlinkedFilesModel.item(row, ReplacementColumn);
        if (replacement && !replacement->text().isEmpty() &&
                m_unlinkedFilesModel.item(row, MissingColumn)->text() == m_service.resource.filePath()) {
            m_service.resource.setFile(replacement->text());
            m_service.newDetail = replacement->text();
            m_service.newHash = replacement->data(ShotcutHashRole).toString();
            m_newXml.writeCharacters(replacement->text());
            m_isCorrected = true;
            return true;
        }
    }
    return false;
}

bool MltXmlChecker::fixShotcutHashProperty()
{
    if (m_xml.attributes().value("name") == kShotcutHashProperty && !m_service.newHash.isEmpty()) {
        m_newXml.writeAttributes(m_xml.attributes());
        m_service.hash = m_xml.readElementText();
        m_newXml.writeCharacters(m_service.newHash);
        m_newXml.writeEndElement();
        return true;
    }
    return false;
}

bool MltXmlChecker::fixShotcutDetailProperty()
{
    if (m_xml.attributes().value("name") == kShotcutCaptionProperty && !m_service.newDetail.isEmpty()) {
        m_newXml.writeAttributes(m_xml.attributes());
        m_newXml.writeCharacters(Util::baseName(m_service.newDetail));
        m_newXml.writeEndElement();
        m_xml.readElementText();
        return true;
    }
    return false;
}

bool MltXmlChecker::fixShotcutCaptionProperty()
{
    if (m_xml.attributes().value("name") == kShotcutDetailProperty && !m_service.newDetail.isEmpty()) {
        m_newXml.writeAttributes(m_xml.attributes());
        m_newXml.writeCharacters(m_service.newDetail);
        m_newXml.writeEndElement();
        m_xml.readElementText();
        return true;
    }
    return false;
}

bool MltXmlChecker::fixAudioIndexProperty()
{
    if (m_xml.attributes().value("name") == "audio_index"
            && !m_service.hash.isEmpty() && !m_service.newHash.isEmpty()
            && m_service.hash != m_service.newHash) {
        m_newXml.writeAttributes(m_xml.attributes());
        m_newXml.writeEndElement();
        m_xml.readElementText();
        return true;
    }
    return false;
}

bool MltXmlChecker::fixVideoIndexProperty()
{
    if (m_xml.attributes().value("name") == "video_index"
            && !m_service.hash.isEmpty() && !m_service.newHash.isEmpty()
            && m_service.hash != m_service.newHash) {
        m_newXml.writeAttributes(m_xml.attributes());
        m_newXml.writeEndElement();
        m_xml.readElementText();
        return true;
    }
    return false;
}
