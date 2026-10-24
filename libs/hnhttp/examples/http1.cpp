#include <hnhttp.hpp>

namespace hnhttp_examples {
using namespace hnhttp;

/* Compile-only design example; multipart is not implemented in the first HTTP/1 slice. */
asyncpp::task<std::shared_ptr<IHttpServerContext>> parseRequest(std::shared_ptr<IHttpStream> stream) {
    auto context = IHttpServerContext::create();
    if (context->setStream(stream) != ERET_OK) { co_return nullptr; }
    auto request = context->getRequest();
    if (co_await request->parseOnce() != ERET_OK) { co_return nullptr; }
    auto security = context->transportSecurity();
    // --> Supplied libcertpp certificates are evidence from transport, not validation by HTTP.
    auto peerCertificates = security.peerCertificates;
    (void)peerCertificates;
    context->getResponse()->status(404, "Not Found");
    co_return context;
}

/* Explicit completion protects connection read-ahead and never silently discards an unlimited body. */
asyncpp::task<ERetCode> replyAndFinish(std::shared_ptr<IHttpServerContext> context) {
    auto sent = co_await context->getResponse()->exec();
    if (sent != ERET_OK) { co_return sent; }
    SFinishOptions finish;
    finish.discardUnreadBody = true;
    finish.maximumDiscardBytes = 1024*1024;
    auto completed = co_await context->finish(finish);
    if (completed != ERET_OK) { co_await context->abort(); }
    co_return completed;
}

/* Retain the connection, not a bare stream, when constructing the next request context. */
asyncpp::task<ERetCode> createRequest(std::shared_ptr<IHttpConnection> connection) {
    auto context = IHttpClientContext::create(std::move(connection));
    auto request = context->getRequest();
    auto body = IFormData::create();
    body->addField("message", "hello").addField("page", 1);
    request->path("/path/to/content");
    // --> The supplied stream cannot infer the required HTTP authority/Host.
    request->authority("example.com");
    request->queries().set("user_id", "somebody").set("page", 1);
    request->body(body).method(HTTP_POST);
    request->headers().set("Authorization", "bearer abcd1234", EINDEX_NEVER)
        .set("X-NonStandard-Header", "test1234");
    auto sent = co_await request->exec();
    if (sent != ERET_OK) { co_return sent; }
    auto response = context->getResponse();
    auto entity = co_await response->getBody()->collect(1024*1024);
    if (entity.result.code != ERET_OK) { co_return entity.result.code; }
    auto uploaded = co_await request->waitSent();
    if (uploaded != ERET_OK) { co_return uploaded; }
    co_return co_await context->finish();
}

/* URL-encoded fields and raw binary use distinct representations. */
void selectBodies(IHttpClientRequest& request) {
    auto fields = IFormData::create(EFORM_URLENCODED);
    fields->addField("query", "a+b c");
    request.body(fields);
    request.body(IHttpBodySource::fromBytes(SBytes{0,1,2,255}));
}

} // namespace hnhttp_examples
