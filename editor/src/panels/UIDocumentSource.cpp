#include "panels/UIDocumentSource.h"

#include "JsonUtil.h"

#include <fstream>
#include <sstream>

namespace VengEditor
{
    using namespace Veng;

    optional<string> SetNthImageSrc(string text, const usize n, const string& idHex)
    {
        usize pos = text.find("<Image");
        for (usize i = 0; i < n && pos != string::npos; ++i)
        {
            pos = text.find("<Image", pos + 6);
        }
        if (pos == string::npos)
        {
            return std::nullopt;
        }
        const usize tagEnd = text.find('>', pos);
        if (tagEnd == string::npos)
        {
            return std::nullopt;
        }
        const usize srcPos = text.find("src=\"", pos);
        if (srcPos != string::npos && srcPos < tagEnd)
        {
            const usize valStart = srcPos + 5;
            const usize valEnd = text.find('"', valStart);
            if (valEnd == string::npos || valEnd > tagEnd)
            {
                return std::nullopt;
            }
            text.replace(valStart, valEnd - valStart, idHex);
        }
        else
        {
            text.insert(pos + 6, fmt::format(" src=\"{}\"", idHex));
        }
        return text;
    }

    optional<string> AppendImage(string text)
    {
        const usize close = text.rfind("</");
        if (close == string::npos)
        {
            return std::nullopt;
        }
        text.insert(close,
                    "  <Image style=\"width: 48px; height: 48px; background: #33445588;\"/>\n");
        return text;
    }

    VoidResult UIDocumentSource::Load(const path& file)
    {
        const std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            return std::unexpected(fmt::format("failed to read {}", file.string()));
        }

        std::ostringstream buffer;
        buffer << in.rdbuf();
        m_Text = buffer.str();
        return {};
    }

    VoidResult UIDocumentSource::Write(const path& file) const
    {
        return WriteTextFile(file, m_Text);
    }

    bool UIDocumentSource::Edit(const function<optional<string>(const string&)>& edit)
    {
        const optional<string> edited = edit(m_Text);
        if (!edited || *edited == m_Text)
        {
            return false;
        }
        m_Text = *edited;
        return true;
    }
}
