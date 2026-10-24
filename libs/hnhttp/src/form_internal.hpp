#ifndef __SRC_HNHTTP_FORM_INTERNAL_HPP__
#define __SRC_HNHTTP_FORM_INTERNAL_HPP__
#include <hnhttp/form.hpp>
namespace hnhttp::detail {
std::shared_ptr<IFormData> createMultipart(SFormOptions options);
void validateFormOptions(const SFormOptions& options);
}
#endif
