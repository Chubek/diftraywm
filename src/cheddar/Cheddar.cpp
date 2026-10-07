#include "cheddar/Cheddar.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cstdlib>

namespace diftray::cheddar {

std::uint64_t LspClient::request(std::string method, std::string params) {
  const auto id = next_id_++;
  std::string body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                     ",\"method\":\"" + method + "\",\"params\":" + params + "}";
  if (send_) send_("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  return id;
}
void LspClient::feed(std::string_view bytes) {
  input_.append(bytes);
  for (;;) {
    const auto split = input_.find("\r\n\r\n"); if (split == std::string::npos) return;
    std::size_t length = 0; const auto header = input_.substr(0, split);
    const auto pos = header.find("Content-Length:");
    if (pos != std::string::npos) length = std::strtoull(header.c_str() + pos + 15, nullptr, 10);
    if (input_.size() < split + 4 + length) return;
    const std::string body = input_.substr(split + 4, length); input_.erase(0, split + 4 + length);
    const auto idpos = body.find("\"id\""); if (idpos == std::string::npos) continue;
    const auto colon = body.find(':', idpos); if (colon == std::string::npos) continue;
    const auto end = body.find_first_of(",}", colon + 1); if (end == std::string::npos) continue;
    try { responses_[std::stoull(body.substr(colon + 1, end - colon - 1))] = body; } catch (...) {}
  }
}
std::optional<std::string> LspClient::take_response(std::uint64_t id) { auto it=responses_.find(id); if(it==responses_.end())return std::nullopt; auto out=std::move(it->second); responses_.erase(it); return out; }

PieceTable::PieceTable(std::string text) { reset(std::move(text)); }
void PieceTable::load_without_history(std::string text) { original_ = std::move(text); added_.clear(); pieces_.clear(); if (!original_.empty()) pieces_.push_back({false, 0, original_.size()}); }
void PieceTable::reset(std::string text) { load_without_history(std::move(text)); undo_text_.clear(); redo_text_.clear(); }
std::size_t PieceTable::size() const { std::size_t n = 0; for (auto p : pieces_) n += p.length; return n; }
std::string PieceTable::materialize() const { std::string out; out.reserve(size()); for (auto p : pieces_) out.append((p.added ? added_ : original_), p.offset, p.length); return out; }
std::string PieceTable::str() const { return materialize(); }
std::size_t PieceTable::line_count() const { const auto text = str(); return 1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')); }
std::string PieceTable::line(std::size_t index) const { const auto text = str(); std::size_t start = 0; for (std::size_t i = 0; i < index; ++i) { auto p = text.find('\n', start); if (p == std::string::npos) return {}; start = p + 1; } auto end = text.find('\n', start); return text.substr(start, end == std::string::npos ? std::string::npos : end - start); }
void PieceTable::remember() { undo_text_.push_back(materialize()); redo_text_.clear(); }
void PieceTable::split(std::size_t position) { if (position == 0 || position >= size()) return; std::size_t at = 0; for (std::size_t i = 0; i < pieces_.size(); ++i) { auto &p = pieces_[i]; if (position > at && position < at + p.length) { const auto left = position - at; pieces_.insert(pieces_.begin() + static_cast<long>(i) + 1, Piece{p.added, p.offset + left, p.length - left}); p.length = left; return; } at += p.length; } }
void PieceTable::insert(std::size_t position, std::string_view text) { if (text.empty()) return; remember(); const auto pos = std::min(position, size()); split(pos); const auto off = added_.size(); added_.append(text); std::size_t at = 0, i = 0; while (i < pieces_.size() && at < pos) { at += pieces_[i++].length; } pieces_.insert(pieces_.begin() + static_cast<long>(i), Piece{true, off, text.size()}); }
void PieceTable::erase(std::size_t position, std::size_t length) { if (!length || position >= size()) return; remember(); const auto end = std::min(size(), position + length); split(end); split(position); std::size_t at = 0; auto first = pieces_.begin(); while (first != pieces_.end() && at < position) { at += first->length; ++first; } auto last = first; while (last != pieces_.end() && at < end) { at += last->length; ++last; } pieces_.erase(first, last); }
bool PieceTable::undo() { if (undo_text_.empty()) return false; redo_text_.push_back(materialize()); auto text=std::move(undo_text_.back()); undo_text_.pop_back(); load_without_history(std::move(text)); return true; }
bool PieceTable::redo() { if (redo_text_.empty()) return false; undo_text_.push_back(materialize()); auto text=std::move(redo_text_.back()); redo_text_.pop_back(); load_without_history(std::move(text)); return true; }

static bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }
LanguageProfile shell_profile() {
  LanguageProfile p; p.name = "diftray-shell"; p.line_comment = "#";
  p.keywords = {"if","then","else","fi","for","while","do","done","case","esac","in","function","time","until"};
  p.builtins = {"cd","pwd","echo","printf","export","set","unset","alias","source","history","jobs","fg","bg","wait","read","test","true","false"};
  p.commands = {"spawn","kill","move","cell","cursor","cursors","workspace","tab","theme","notelet","help","cheddar","dispatch","set","launch"};
  p.highlight = [p](std::string_view s) { std::vector<Highlight> out; for (std::size_t i = 0; i < s.size();) { if (s[i] == '#') { out.push_back({i, s.size()-i, "comment"}); break; } if (s[i] == '$') { std::size_t j=i+1; while (j<s.size() && word_char(s[j])) ++j; out.push_back({i,j-i,"variable"}); i=j; continue; } if (s[i]=='\'' || s[i]=='\"') { char q=s[i]; auto j=s.find(q,i+1); j=j==std::string_view::npos?s.size():j+1; out.push_back({i,j-i,"string"}); i=j; continue; } if (word_char(s[i])) { auto j=i+1; while(j<s.size()&&word_char(s[j]))++j; std::string w(s.substr(i,j-i)); auto kind = std::find(p.keywords.begin(),p.keywords.end(),w)!=p.keywords.end()?"keyword":std::find(p.builtins.begin(),p.builtins.end(),w)!=p.builtins.end()?"builtin":std::find(p.commands.begin(),p.commands.end(),w)!=p.commands.end()?"command":"identifier"; out.push_back({i,j-i,kind}); i=j; continue; } ++i; } return out; };
  p.indent = [](std::string_view s, std::size_t previous) { std::size_t n=previous; if (s.find("then")!=std::string_view::npos || s.find("do")!=std::string_view::npos) n+=2; if (s.find("fi")!=std::string_view::npos || s.find("done")!=std::string_view::npos || s.find("esac")!=std::string_view::npos) n=n>=2?n-2:0; return n; };
  p.complete = [p](std::string_view s, std::size_t col) { std::size_t b=col; while(b&&word_char(s[b-1]))--b; std::string prefix(s.substr(b,col-b)); std::vector<Completion> out; auto add=[&](const std::vector<std::string>& xs,std::string d){for(auto &x:xs)if(x.rfind(prefix,0)==0)out.push_back({x,x,d});}; add(p.commands,"Diftray command"); add(p.builtins,"shell builtin"); add(p.keywords,"shell keyword"); return out; };
  return p;
}
LanguageProfile profile_for_path(const std::filesystem::path &path) { return path.extension()==".sh" || path.filename()==".profile" ? shell_profile() : shell_profile(); }

bool Editor::open(const std::filesystem::path &path, std::string *error) { std::ifstream in(path); if (!in && std::filesystem::exists(path)) { if(error)*error="cannot open "+path.string(); return false; } std::stringstream ss; ss<<in.rdbuf(); path_=path; buffer_.reset(ss.str()); saved_=buffer_; profile_=profile_for_path(path); active_=true; dispatch_mode_=false; return true; }
bool Editor::save(std::string *error) { if(!active_){if(error)*error="editor is not open";return false;} std::ofstream out(path_); if(!out){if(error)*error="cannot write "+path_.string();return false;} out<<buffer_.str(); saved_=buffer_; return true; }
void Editor::close(){active_=false;dispatch_mode_=false;path_.clear();buffer_.reset({});saved_.reset({});}
std::vector<Highlight> Editor::highlights(std::size_t l) const { return profile_.highlight ? profile_.highlight(buffer_.line(l)) : std::vector<Highlight>{}; }
std::vector<Completion> Editor::complete(std::size_t l,std::size_t c) const { auto s=buffer_.line(l); return profile_.complete ? profile_.complete(s,std::min(c,s.size())) : std::vector<Completion>{}; }
void Editor::map_key(std::string k,std::string a){keys_[std::move(k)]=std::move(a);} void Editor::shortcut(std::string k,std::string c){shortcuts_[std::move(k)]=std::move(c);} void Editor::abbreviation(std::string f,std::string t){abbreviations_[std::move(f)]=std::move(t);}
std::optional<std::string> Editor::key_action(std::string_view k) const { auto i=keys_.find(k); return i==keys_.end()?std::nullopt:std::optional<std::string>(i->second); }
std::optional<std::string> Editor::expand(std::string_view w) const { auto i=abbreviations_.find(w); return i==abbreviations_.end()?std::nullopt:std::optional<std::string>(i->second); }
std::string Editor::diff() const { if(!active_) return {}; if(buffer_.str()==saved_.str()) return {}; return "--- " + path_.string() + "\n+++ " + path_.string() + "\n@@ modified @@\n" + buffer_.str(); }
}
