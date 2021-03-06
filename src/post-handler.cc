#include <mimosa/http/redirect.hh>
#include <mimosa/http/server-channel.hh>
#include <mimosa/log/log.hh>
#include <mimosa/net/print.hh>
#include <mimosa/sqlite/sqlite.hh>
#include <mimosa/tpl/dict.hh>
#include <mimosa/stream/lzma-encoder.hh>
#include <mimosa/stream/string-stream.hh>

#include "bottleneck.hh"
#include "config.hh"
#include "db.hh"
#include "encoding.hh"
#include "error-handler.hh"
#include "load-tpl.hh"
#include "page-footer.hh"
#include "page-header.hh"
#include "post-handler.hh"
#include "purge.hh"

static void
encode(const std::string & input,
       std::string * output,
       Encoding * encoding)
{
  if (input.size() < 512)
  {
    *output = input;
    *encoding = kIdentity;
    return;
  }

  *encoding = kLzma;
  mimosa::stream::StringStream::Ptr str = new mimosa::stream::StringStream;
  mimosa::stream::LzmaEncoder::Ptr lzma = new mimosa::stream::LzmaEncoder(str);
  lzma->loopWrite(input.data(), input.size());
  lzma->flush();
  *output = str->str();
}

bool
PostHandler::handle(mimosa::http::RequestReader & request,
                    mimosa::http::ResponseWriter & response) const
{
  const auto & form = request.form();

  auto it = form.find("content");
  if (it != form.end())
  {
    if (it->second.size() > Config::maxPasteSize())
      return errorHandler(response, "post size exceed limit");

    Encoding encoding;
    std::string content;

    encode(it->second, &content, &encoding);

    mimosa::sqlite::Stmt stmt;
    int err = stmt.prepare(Db::handle(),
                           "insert or fail into paste (content, ip, encoding)"
                           " values (?, ?, ?)");
    if (err != SQLITE_OK)
      return errorHandler(response, "sqlite error");

    err = stmt.bindBlob(1, content.data(), content.size());
    if (err != SQLITE_OK)
      return errorHandler(response, "sqlite error");

    err = stmt.bind(2, mimosa::net::print(request.channel().remoteAddr(),
                                          request.channel().remoteAddrLen()));
    if (err != SQLITE_OK)
      return errorHandler(response, "sqlite error");

    err = stmt.bind(3, encoding);
    if (err != SQLITE_OK)
      return errorHandler(response, "sqlite error");

    err = stmt.step();
    if (err != SQLITE_DONE)
      return errorHandler(response, "sqlite error");

    int64_t row_id = sqlite3_last_insert_rowid(Db::handle());
    Purge::instance().newPaste(it->second.size());
    return redirect(response, mimosa::format::str("/view?id=%d", row_id));
  }

  auto tpl = loadTpl("page.html");
  if (!tpl)
    return false;

  mimosa::tpl::Dict dict;

  auto tpl_body = loadTpl("post.html");
  if (!tpl_body)
    return false;
  dict.append("body", tpl_body);

  setPageHeader(dict);
  setPageFooter(dict);

  response.status_ = mimosa::http::kStatusOk;
  response.content_type_ = "text/html";
  response.sendHeader(response.writeTimeout());
  tpl->execute(&response, dict, response.writeTimeout());
  return true;
}
